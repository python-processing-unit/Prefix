#include "../src/prefix_extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#else
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include "../src/interpreter.h"

typedef struct {
	int id;
	SOCKET sock;
} NetHandle;

typedef struct {
	NetHandle* tcp;
	size_t tcp_count;
	size_t tcp_cap;
	NetHandle* udp;
	size_t udp_count;
	size_t udp_cap;
	int next_id;
	int sockets_ready;
} NetState;

static NetState g_state = {0};

#define RUNTIME_ERROR(interp, msg, line, col) \
	do { \
		(interp)->error = strdup(msg); \
		(interp)->error_line = (line); \
		(interp)->error_col = (col); \
		return value_null(); \
	} while (0)

#define EXPECT_INT_AT(args, idx, opname, interp, line, col) \
	do { \
		if ((idx) >= argc || (args)[(idx)].type != VAL_INT) { \
			char _buf[128]; \
			snprintf(_buf, sizeof(_buf), "%s expects INT", opname); \
			RUNTIME_ERROR(interp, _buf, line, col); \
		} \
	} while (0)

#define EXPECT_STR_AT(args, idx, opname, interp, line, col) \
	do { \
		if ((idx) >= argc || (args)[(idx)].type != VAL_STR) { \
			char _buf[128]; \
			snprintf(_buf, sizeof(_buf), "%s expects STR", opname); \
			RUNTIME_ERROR(interp, _buf, line, col); \
		} \
	} while (0)

#define EXPECT_ARGC_MINMAX(opname, minv, maxv) \
	do { \
		if (argc < (minv) || argc > (maxv)) { \
			char _buf[160]; \
			snprintf(_buf, sizeof(_buf), "%s expects %d..%d arguments", opname, (int)(minv), (int)(maxv)); \
			RUNTIME_ERROR(interp, _buf, line, col); \
		} \
	} while (0)

static int64_t as_i64(Value v) { return (int64_t)v.as.i; }
static const char* as_cstr(Value v) { return v.as.s ? v.as.s : ""; }

static void ensure_socket_runtime(void) {
	if (g_state.sockets_ready) return;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
		g_state.sockets_ready = 1;
	}
#else
	g_state.sockets_ready = 1;
#endif
	if (g_state.next_id <= 0) g_state.next_id = 1;
}

static int ms_to_timeout_ms(int64_t timeout_ms) {
	if (timeout_ms <= 0) return -1;
	if (timeout_ms > 2147483647LL) return 2147483647;
	return (int)timeout_ms;
}

static int set_socket_timeout_ms(SOCKET s, int timeout_ms) {
	if (timeout_ms < 0) return 0;
#ifdef _WIN32
	DWORD tv = (DWORD)timeout_ms;
	if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) != 0) return -1;
	if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) != 0) return -1;
#else
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
	if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
#endif
	return 0;
}

static int reserve_handles(NetHandle** arr, size_t* cap, size_t need) {
	if (*cap >= need) return 0;
	size_t next = (*cap == 0) ? 8 : (*cap * 2);
	while (next < need) next *= 2;
	NetHandle* p = (NetHandle*)realloc(*arr, next * sizeof(NetHandle));
	if (!p) return -1;
	*arr = p;
	*cap = next;
	return 0;
}

static int add_handle(NetHandle** arr, size_t* count, size_t* cap, SOCKET sock) {
	if (reserve_handles(arr, cap, *count + 1) != 0) return -1;
	int id = g_state.next_id++;
	(*arr)[*count].id = id;
	(*arr)[*count].sock = sock;
	(*count)++;
	return id;
}

static SOCKET find_handle(NetHandle* arr, size_t count, int id) {
	for (size_t i = 0; i < count; i++) {
		if (arr[i].id == id) return arr[i].sock;
	}
	return INVALID_SOCKET;
}

static int remove_handle(NetHandle* arr, size_t* count, int id, SOCKET* out) {
	for (size_t i = 0; i < *count; i++) {
		if (arr[i].id == id) {
			*out = arr[i].sock;
			arr[i] = arr[*count - 1];
			(*count)--;
			return 0;
		}
	}
	return -1;
}

static Value bytes_to_tns(const unsigned char* data, size_t len) {
	size_t shape = (len == 0) ? 1 : len;
	Value* items = (Value*)malloc(sizeof(Value) * shape);
	if (!items) return value_null();
	if (len == 0) {
		items[0] = value_int(0);
	} else {
		for (size_t i = 0; i < len; i++) items[i] = value_int((int64_t)data[i]);
	}
	Value t = value_tns_from_values(TYPE_INT, 1, &shape, items, shape);
	free(items);
	return t;
}

static int tns_to_bytes(Value v, unsigned char** out_data, size_t* out_len) {
	if (v.type != VAL_TNS || !v.as.tns) return -1;
	Tensor* t = v.as.tns;
	if (t->ndim != 1) return -1;
	unsigned char* buf = (unsigned char*)malloc(t->length == 0 ? 1 : t->length);
	if (!buf) return -1;
	for (size_t i = 0; i < t->length; i++) {
		Value e = t->data[i];
		if (e.type != VAL_INT) {
			free(buf);
			return -1;
		}
		int64_t b = e.as.i;
		if (b < 0 || b > 255) {
			free(buf);
			return -1;
		}
		buf[i] = (unsigned char)b;
	}
	*out_data = buf;
	*out_len = t->length;
	return 0;
}

static char* normalize_encoding_name(const char* coding) {
	if (!coding || coding[0] == '\0') return strdup("UTF-8");
	if (_stricmp(coding, "UTF8") == 0 || _stricmp(coding, "UTF-8") == 0) return strdup("UTF-8");
	if (_stricmp(coding, "UTF16") == 0 || _stricmp(coding, "UTF-16") == 0) return strdup("UTF-16");
	if (_stricmp(coding, "ASCII") == 0) return strdup("ASCII");
	if (_stricmp(coding, "LATIN1") == 0 || _stricmp(coding, "LATIN-1") == 0) return strdup("latin-1");
	if (_stricmp(coding, "ANSI") == 0) return strdup("cp1252");
	return strdup(coding);
}

static char* hex_encode(const unsigned char* data, size_t len) {
	static const char* hex = "0123456789abcdef";
	char* out = (char*)malloc(len * 2 + 1);
	if (!out) return NULL;
	for (size_t i = 0; i < len; i++) {
		out[i * 2] = hex[(data[i] >> 4) & 0xF];
		out[i * 2 + 1] = hex[data[i] & 0xF];
	}
	out[len * 2] = '\0';
	return out;
}

static int write_text_file(const char* path, const char* data) {
	FILE* f = fopen(path, "wb");
	if (!f) return -1;
	if (data && data[0]) fwrite(data, 1, strlen(data), f);
	fclose(f);
	return 0;
}

static unsigned char* read_file_bytes(const char* path, size_t* out_len) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long sz = ftell(f);
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	unsigned char* data = (unsigned char*)malloc((size_t)sz + 1);
	if (!data) {
		fclose(f);
		return NULL;
	}
	size_t n = fread(data, 1, (size_t)sz, f);
	fclose(f);
	data[n] = 0;
	*out_len = n;
	return data;
}

static const char* py_bridge_script(void) {
	return
		"import sys,ssl,io,urllib.request,ftplib,smtplib\n"
		"def d(x): return bytes.fromhex(x).decode('utf-8','surrogatepass')\n"
		"def fail(m):\n"
		"  sys.stderr.write(str(m)); sys.exit(2)\n"
		"if len(sys.argv)!=3: fail('bridge args')\n"
		"reqp,rspp=sys.argv[1],sys.argv[2]\n"
		"with open(reqp,'r',encoding='utf-8',errors='surrogatepass') as f:\n"
		"  lines=[ln.rstrip('\\n') for ln in f]\n"
		"if not lines: fail('empty request')\n"
		"op=lines[0]\n"
		"args=[d(x) for x in lines[1:]]\n"
		"def wbytes(b):\n"
		"  with open(rspp,'wb') as f: f.write(b)\n"
		"def wtext(s): wbytes(str(s).encode('utf-8','surrogatepass'))\n"
		"try:\n"
		"  if op=='FTP_LIST':\n"
		"    host,port,user,pwd,dirp,tls,timeout_ms,verify=args\n"
		"    port=int(port); tls=int(tls); verify=int(verify); tm=int(timeout_ms)\n"
		"    t=None if tm<=0 else tm/1000.0\n"
		"    if tls:\n"
		"      ftp=ftplib.FTP_TLS(); ftp.context=(ssl.create_default_context() if verify else ssl._create_unverified_context())\n"
		"      ftp.connect(host=host,port=port,timeout=t); ftp.login(user=user,passwd=pwd); ftp.prot_p()\n"
		"    else:\n"
		"      ftp=ftplib.FTP(); ftp.connect(host=host,port=port,timeout=t); ftp.login(user=user,passwd=pwd)\n"
		"    lines=[]\n"
		"    try:\n"
		"      ftp.retrlines('LIST ' + dirp, callback=lines.append)\n"
		"    finally:\n"
		"      try: ftp.quit()\n"
		"      except Exception: ftp.close()\n"
		"    wtext('\\n'.join(lines))\n"
		"  elif op=='FTP_GET_BYTES':\n"
		"    host,port,user,pwd,path,tls,timeout_ms,verify=args\n"
		"    port=int(port); tls=int(tls); verify=int(verify); tm=int(timeout_ms)\n"
		"    t=None if tm<=0 else tm/1000.0\n"
		"    if tls:\n"
		"      ftp=ftplib.FTP_TLS(); ftp.context=(ssl.create_default_context() if verify else ssl._create_unverified_context())\n"
		"      ftp.connect(host=host,port=port,timeout=t); ftp.login(user=user,passwd=pwd); ftp.prot_p()\n"
		"    else:\n"
		"      ftp=ftplib.FTP(); ftp.connect(host=host,port=port,timeout=t); ftp.login(user=user,passwd=pwd)\n"
		"    buf=io.BytesIO()\n"
		"    try:\n"
		"      ftp.retrbinary('RETR ' + path, callback=buf.write)\n"
		"    finally:\n"
		"      try: ftp.quit()\n"
		"      except Exception: ftp.close()\n"
		"    wbytes(buf.getvalue())\n"
		"  elif op=='FTP_PUT_BYTES':\n"
		"    host,port,user,pwd,path,data_hex,tls,timeout_ms,verify=args\n"
		"    port=int(port); tls=int(tls); verify=int(verify); tm=int(timeout_ms)\n"
		"    t=None if tm<=0 else tm/1000.0\n"
		"    data=bytes.fromhex(data_hex)\n"
		"    if tls:\n"
		"      ftp=ftplib.FTP_TLS(); ftp.context=(ssl.create_default_context() if verify else ssl._create_unverified_context())\n"
		"      ftp.connect(host=host,port=port,timeout=t); ftp.login(user=user,passwd=pwd); ftp.prot_p()\n"
		"    else:\n"
		"      ftp=ftplib.FTP(); ftp.connect(host=host,port=port,timeout=t); ftp.login(user=user,passwd=pwd)\n"
		"    try:\n"
		"      ftp.storbinary('STOR ' + path, io.BytesIO(data))\n"
		"    finally:\n"
		"      try: ftp.quit()\n"
		"      except Exception: ftp.close()\n"
		"    wtext('1')\n"
		"  elif op=='SMTP_SEND':\n"
		"    host,port,user,pwd,mfrom,mto,subj,body,tls,timeout_ms,verify=args\n"
		"    port=int(port); tls=int(tls); verify=int(verify); tm=int(timeout_ms)\n"
		"    t=None if tm<=0 else tm/1000.0\n"
		"    rec=[r.strip() for r in mto.replace(';',',').split(',') if r.strip()]\n"
		"    if not rec: fail('SMTP_SEND: no recipients')\n"
		"    msg='\\r\\n'.join([f'From: {mfrom}','To: ' + ', '.join(rec),f'Subject: {subj}','MIME-Version: 1.0','Content-Type: text/plain; charset=utf-8','',''+body,''])\n"
		"    if tls:\n"
		"      ctx=ssl.create_default_context() if verify else ssl._create_unverified_context()\n"
		"      c=smtplib.SMTP_SSL(host=host,port=port,timeout=t,context=ctx)\n"
		"    else:\n"
		"      c=smtplib.SMTP(host=host,port=port,timeout=t)\n"
		"    try:\n"
		"      c.ehlo()\n"
		"      if user or pwd: c.login(user,pwd)\n"
		"      c.sendmail(mfrom,rec,msg)\n"
		"    finally:\n"
		"      try: c.quit()\n"
		"      except Exception: c.close()\n"
		"    wtext('1')\n"
		"  else:\n"
		"    fail('unknown op')\n"
		"except Exception as e:\n"
		"  fail(op + ' failed: ' + str(e))\n";
}

static int ensure_bridge_script(char* out_path, size_t out_path_cap) {
#ifndef _WIN32
    (void)out_path;
    (void)out_path_cap;
    return -1;
#else
	char temp_dir[MAX_PATH];
	DWORD n = GetTempPathA((DWORD)sizeof(temp_dir), temp_dir);
	if (n == 0 || n >= sizeof(temp_dir)) return -1;
	char script_path[MAX_PATH];
	if (snprintf(script_path, sizeof(script_path), "%s%s", temp_dir, "prefix_networking_bridge.py") < 0) return -1;
	const char* code = py_bridge_script();
	if (write_text_file(script_path, code) != 0) return -1;
	strncpy_s(out_path, out_path_cap, script_path, out_path_cap - 1);
	return 0;
#endif
}

static int create_temp_file_path(char* out_path, size_t out_cap) {
#ifndef _WIN32
    (void)out_path;
    (void)out_cap;
    return -1;
#else
	char temp_dir[MAX_PATH];
	DWORD n = GetTempPathA((DWORD)sizeof(temp_dir), temp_dir);
	if (n == 0 || n >= sizeof(temp_dir)) return -1;
	char temp_file[MAX_PATH];
	if (GetTempFileNameA(temp_dir, "pfx", 0, temp_file) == 0) return -1;
	strncpy_s(out_path, out_cap, temp_file, out_cap - 1);
	return 0;
#endif
}

static int bridge_call(const char* op, const char** args, int arg_count, unsigned char** out_data, size_t* out_len, char** out_err) {
#ifndef _WIN32
	(void)op;
	(void)args;
	(void)arg_count;
	(void)out_data;
	(void)out_len;
	*out_err = strdup("bridge unsupported on this platform");
	return -1;
#else
	char script_path[MAX_PATH];
	char req_path[MAX_PATH];
	char rsp_path[MAX_PATH];
	if (ensure_bridge_script(script_path, sizeof(script_path)) != 0) {
		*out_err = strdup("bridge: failed to prepare Python script");
		return -1;
	}
	if (create_temp_file_path(req_path, sizeof(req_path)) != 0 || create_temp_file_path(rsp_path, sizeof(rsp_path)) != 0) {
		*out_err = strdup("bridge: failed to allocate temp files");
		return -1;
	}

	FILE* rf = fopen(req_path, "wb");
	if (!rf) {
		*out_err = strdup("bridge: failed to open request file");
		remove(req_path);
		remove(rsp_path);
		return -1;
	}
	fprintf(rf, "%s\n", op);
	for (int i = 0; i < arg_count; i++) {
		const char* s = args[i] ? args[i] : "";
		char* hex = hex_encode((const unsigned char*)s, strlen(s));
		if (!hex) {
			fclose(rf);
			remove(req_path);
			remove(rsp_path);
			*out_err = strdup("bridge: out of memory");
			return -1;
		}
		fprintf(rf, "%s\n", hex);
		free(hex);
	}
	fclose(rf);

	char cmd[4096];
	snprintf(cmd, sizeof(cmd), "python \"%s\" \"%s\" \"%s\" 2>&1", script_path, req_path, rsp_path);
	FILE* p = _popen(cmd, "rb");
	if (!p) {
		remove(req_path);
		remove(rsp_path);
		*out_err = strdup("bridge: failed to start python");
		return -1;
	}

	size_t log_cap = 256;
	size_t log_len = 0;
	char* log = (char*)malloc(log_cap);
	if (!log) {
		_pclose(p);
		remove(req_path);
		remove(rsp_path);
		*out_err = strdup("bridge: out of memory");
		return -1;
	}
	log[0] = '\0';
	char chunk[256];
	while (!feof(p)) {
		size_t nread = fread(chunk, 1, sizeof(chunk), p);
		if (nread == 0) break;
		if (log_len + nread + 1 > log_cap) {
			size_t nc = log_cap * 2;
			while (nc < log_len + nread + 1) nc *= 2;
			char* np = (char*)realloc(log, nc);
			if (!np) break;
			log = np;
			log_cap = nc;
		}
		memcpy(log + log_len, chunk, nread);
		log_len += nread;
		log[log_len] = '\0';
	}
	int rc = _pclose(p);

	if (rc != 0) {
		*out_err = strdup((log_len > 0) ? log : "bridge: python call failed");
		free(log);
		remove(req_path);
		remove(rsp_path);
		return -1;
	}
	free(log);

	size_t rlen = 0;
	unsigned char* rb = read_file_bytes(rsp_path, &rlen);
	remove(req_path);
	remove(rsp_path);
	if (!rb) {
		*out_err = strdup("bridge: no response");
		return -1;
	}
	*out_data = rb;
	*out_len = rlen;
	return 0;
#endif
}

#ifdef _WIN32
static wchar_t* utf8_to_wide(const char* s) {
	if (!s) s = "";
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) return NULL;
	wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)n);
	if (!w) return NULL;
	if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
		free(w);
		return NULL;
	}
	return w;
}

static int winhttp_request(const char* method_u8, const char* url_u8, const unsigned char* body, size_t body_len,
						   const char* content_type_u8, int timeout_ms, int verify,
						   int* out_status, unsigned char** out_body, size_t* out_body_len) {
	int ret = -1;
	HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
	wchar_t* wmethod = NULL;
	wchar_t* wurl = NULL;
	wchar_t host[256];
	wchar_t path[2048];
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));
	uc.dwSchemeLength = 1;

	wmethod = utf8_to_wide(method_u8);
	wurl = utf8_to_wide(url_u8);
	if (!wmethod || !wurl) goto cleanup;

	if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) goto cleanup;
	host[uc.dwHostNameLength] = L'\0';
	path[uc.dwUrlPathLength] = L'\0';

	hSession = WinHttpOpen(L"Prefix-C/networking", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
						   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) goto cleanup;

	if (timeout_ms > 0) {
		WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
	}

	hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
	if (!hConnect) goto cleanup;

	DWORD req_flags = 0;
	if (uc.nScheme == INTERNET_SCHEME_HTTPS) req_flags |= WINHTTP_FLAG_SECURE;
	hRequest = WinHttpOpenRequest(hConnect, wmethod, path, NULL, WINHTTP_NO_REFERER,
								  WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
	if (!hRequest) goto cleanup;

	if (!verify && uc.nScheme == INTERNET_SCHEME_HTTPS) {
		DWORD sec = SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
					SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
					SECURITY_FLAG_IGNORE_UNKNOWN_CA |
					SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
		WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
	}

	wchar_t* content_type_w = NULL;
	wchar_t headers_buf[512];
	LPCWSTR headers_ptr = WINHTTP_NO_ADDITIONAL_HEADERS;
	DWORD headers_len = 0;
	if (content_type_u8 && content_type_u8[0]) {
		content_type_w = utf8_to_wide(content_type_u8);
		if (!content_type_w) goto cleanup;
		_snwprintf_s(headers_buf, sizeof(headers_buf)/sizeof(headers_buf[0]), _TRUNCATE, L"Content-Type: %ls\r\n", content_type_w);
		headers_ptr = headers_buf;
		headers_len = (DWORD)-1;
	}

	if (!WinHttpSendRequest(hRequest,
							headers_ptr,
							headers_len,
							(LPVOID)body,
							(DWORD)body_len,
							(DWORD)body_len,
							0)) {
		free(content_type_w);
		goto cleanup;
	}
	free(content_type_w);

	if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

	DWORD status = 0;
	DWORD status_size = sizeof(status);
	if (!WinHttpQueryHeaders(hRequest,
							 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
							 WINHTTP_HEADER_NAME_BY_INDEX,
							 &status,
							 &status_size,
							 WINHTTP_NO_HEADER_INDEX)) {
		goto cleanup;
	}

	size_t cap = 4096;
	size_t len = 0;
	unsigned char* data = (unsigned char*)malloc(cap);
	if (!data) goto cleanup;

	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
			free(data);
			goto cleanup;
		}
		if (avail == 0) break;
		if (len + (size_t)avail > cap) {
			size_t nc = cap * 2;
			while (nc < len + (size_t)avail) nc *= 2;
			unsigned char* np = (unsigned char*)realloc(data, nc);
			if (!np) {
				free(data);
				goto cleanup;
			}
			data = np;
			cap = nc;
		}
		DWORD read_now = 0;
		if (!WinHttpReadData(hRequest, data + len, avail, &read_now)) {
			free(data);
			goto cleanup;
		}
		len += read_now;
	}

	*out_status = (int)status;
	*out_body = data;
	*out_body_len = len;
	ret = 0;

cleanup:
	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	if (hSession) WinHttpCloseHandle(hSession);
	free(wmethod);
	free(wurl);
	return ret;
}
#endif

static Value op_tcp_connect(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("TCP_CONNECT", 2, 6);
	EXPECT_STR_AT(args, 0, "TCP_CONNECT", interp, line, col);
	EXPECT_INT_AT(args, 1, "TCP_CONNECT", interp, line, col);
	ensure_socket_runtime();

	const char* host = as_cstr(args[0]);
	int64_t port = as_i64(args[1]);
	int64_t timeout_ms = (argc >= 3) ? as_i64(args[2]) : 5000;
	int64_t tls = (argc >= 4) ? as_i64(args[3]) : 0;
	int64_t verify = (argc >= 5) ? as_i64(args[4]) : 1;
	(void)tls;
	(void)verify;
	if (argc >= 3) EXPECT_INT_AT(args, 2, "TCP_CONNECT", interp, line, col);
	if (argc >= 4) EXPECT_INT_AT(args, 3, "TCP_CONNECT", interp, line, col);
	if (argc >= 5) EXPECT_INT_AT(args, 4, "TCP_CONNECT", interp, line, col);
	if (argc >= 6) EXPECT_STR_AT(args, 5, "TCP_CONNECT", interp, line, col);

	if (port < 0 || port > 65535) RUNTIME_ERROR(interp, "TCP_CONNECT: port out of range", line, col);
	if (tls != 0) RUNTIME_ERROR(interp, "TCP_CONNECT: TLS not supported in C extension build", line, col);

	char port_buf[32];
	snprintf(port_buf, sizeof(port_buf), "%lld", (long long)port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	struct addrinfo* res = NULL;
	if (getaddrinfo(host, port_buf, &hints, &res) != 0 || !res) {
		RUNTIME_ERROR(interp, "TCP_CONNECT failed: resolve error", line, col);
	}

	SOCKET s = INVALID_SOCKET;
	for (struct addrinfo* it = res; it; it = it->ai_next) {
		s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (s == INVALID_SOCKET) continue;
		int tmo = ms_to_timeout_ms(timeout_ms);
		set_socket_timeout_ms(s, tmo);
		if (connect(s, it->ai_addr, (int)it->ai_addrlen) == 0) {
			break;
		}
		closesocket(s);
		s = INVALID_SOCKET;
	}
	freeaddrinfo(res);

	if (s == INVALID_SOCKET) {
		RUNTIME_ERROR(interp, "TCP_CONNECT failed", line, col);
	}

	int id = add_handle(&g_state.tcp, &g_state.tcp_count, &g_state.tcp_cap, s);
	if (id <= 0) {
		closesocket(s);
		RUNTIME_ERROR(interp, "TCP_CONNECT failed: out of memory", line, col);
	}
	return value_int(id);
}

static Value op_tcp_send(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("TCP_SEND", 2, 3);
	EXPECT_INT_AT(args, 0, "TCP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 1, "TCP_SEND", interp, line, col);
	if (argc >= 3) EXPECT_STR_AT(args, 2, "TCP_SEND", interp, line, col);

	int hid = (int)as_i64(args[0]);
	const char* payload = as_cstr(args[1]);
	SOCKET s = find_handle(g_state.tcp, g_state.tcp_count, hid);
	if (s == INVALID_SOCKET) RUNTIME_ERROR(interp, "TCP_SEND: invalid handle", line, col);

	const char* coding = (argc >= 3) ? as_cstr(args[2]) : "UTF-8";
	char* norm = normalize_encoding_name(coding);
	if (!norm) RUNTIME_ERROR(interp, "TCP_SEND failed: out of memory", line, col);
	if (_stricmp(norm, "UTF-8") != 0 && _stricmp(norm, "ASCII") != 0 && _stricmp(norm, "latin-1") != 0 && _stricmp(norm, "cp1252") != 0) {
		free(norm);
		RUNTIME_ERROR(interp, "TCP_SEND failed: unsupported coding", line, col);
	}
	free(norm);

	int n = send(s, payload, (int)strlen(payload), 0);
	if (n == SOCKET_ERROR) RUNTIME_ERROR(interp, "TCP_SEND failed", line, col);
	return value_int(n);
}

static Value op_tcp_recv_text(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("TCP_RECV_TEXT", 2, 3);
	EXPECT_INT_AT(args, 0, "TCP_RECV_TEXT", interp, line, col);
	EXPECT_INT_AT(args, 1, "TCP_RECV_TEXT", interp, line, col);
	if (argc >= 3) EXPECT_STR_AT(args, 2, "TCP_RECV_TEXT", interp, line, col);

	int hid = (int)as_i64(args[0]);
	int64_t max_bytes = as_i64(args[1]);
	if (max_bytes <= 0 || max_bytes > 16 * 1024 * 1024) RUNTIME_ERROR(interp, "TCP_RECV_TEXT: max_bytes must be > 0", line, col);
	SOCKET s = find_handle(g_state.tcp, g_state.tcp_count, hid);
	if (s == INVALID_SOCKET) RUNTIME_ERROR(interp, "TCP_RECV_TEXT: invalid handle", line, col);

	char* buf = (char*)malloc((size_t)max_bytes + 1);
	if (!buf) RUNTIME_ERROR(interp, "TCP_RECV_TEXT failed: out of memory", line, col);
	int n = recv(s, buf, (int)max_bytes, 0);
	if (n == SOCKET_ERROR) {
		free(buf);
		RUNTIME_ERROR(interp, "TCP_RECV_TEXT failed", line, col);
	}
	buf[n < 0 ? 0 : n] = '\0';
	Value v = value_str(buf);
	free(buf);
	return v;
}

static Value op_tcp_recv_bytes(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("TCP_RECV_BYTES", 2, 2);
	EXPECT_INT_AT(args, 0, "TCP_RECV_BYTES", interp, line, col);
	EXPECT_INT_AT(args, 1, "TCP_RECV_BYTES", interp, line, col);

	int hid = (int)as_i64(args[0]);
	int64_t max_bytes = as_i64(args[1]);
	if (max_bytes <= 0 || max_bytes > 16 * 1024 * 1024) RUNTIME_ERROR(interp, "TCP_RECV_BYTES: max_bytes must be > 0", line, col);
	SOCKET s = find_handle(g_state.tcp, g_state.tcp_count, hid);
	if (s == INVALID_SOCKET) RUNTIME_ERROR(interp, "TCP_RECV_BYTES: invalid handle", line, col);

	unsigned char* buf = (unsigned char*)malloc((size_t)max_bytes);
	if (!buf) RUNTIME_ERROR(interp, "TCP_RECV_BYTES failed: out of memory", line, col);
	int n = recv(s, (char*)buf, (int)max_bytes, 0);
	if (n == SOCKET_ERROR) {
		free(buf);
		RUNTIME_ERROR(interp, "TCP_RECV_BYTES failed", line, col);
	}
	Value out = bytes_to_tns(buf, (size_t)((n < 0) ? 0 : n));
	free(buf);
	if (out.type == VAL_NULL) RUNTIME_ERROR(interp, "TCP_RECV_BYTES failed: allocation", line, col);
	return out;
}

static Value op_tcp_close(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("TCP_CLOSE", 1, 1);
	EXPECT_INT_AT(args, 0, "TCP_CLOSE", interp, line, col);
	int hid = (int)as_i64(args[0]);
	SOCKET s = INVALID_SOCKET;
	if (remove_handle(g_state.tcp, &g_state.tcp_count, hid, &s) != 0) {
		RUNTIME_ERROR(interp, "TCP_CLOSE: invalid handle", line, col);
	}
	closesocket(s);
	return value_int(0);
}

static Value op_udp_bind(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("UDP_BIND", 2, 3);
	EXPECT_STR_AT(args, 0, "UDP_BIND", interp, line, col);
	EXPECT_INT_AT(args, 1, "UDP_BIND", interp, line, col);
	if (argc >= 3) EXPECT_INT_AT(args, 2, "UDP_BIND", interp, line, col);
	ensure_socket_runtime();

	const char* host = as_cstr(args[0]);
	int64_t port = as_i64(args[1]);
	int64_t timeout_ms = (argc >= 3) ? as_i64(args[2]) : 0;
	if (port < 0 || port > 65535) RUNTIME_ERROR(interp, "UDP_BIND: port out of range", line, col);

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) RUNTIME_ERROR(interp, "UDP_BIND failed", line, col);
	int tmo = ms_to_timeout_ms(timeout_ms);
	set_socket_timeout_ms(s, tmo);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	if (host[0] == '\0' || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
			closesocket(s);
			RUNTIME_ERROR(interp, "UDP_BIND failed: invalid host", line, col);
		}
	}

	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		closesocket(s);
		RUNTIME_ERROR(interp, "UDP_BIND failed", line, col);
	}

	int id = add_handle(&g_state.udp, &g_state.udp_count, &g_state.udp_cap, s);
	if (id <= 0) {
		closesocket(s);
		RUNTIME_ERROR(interp, "UDP_BIND failed: out of memory", line, col);
	}
	return value_int(id);
}

static Value op_udp_send(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("UDP_SEND", 4, 5);
	EXPECT_INT_AT(args, 0, "UDP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 1, "UDP_SEND", interp, line, col);
	EXPECT_INT_AT(args, 2, "UDP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 3, "UDP_SEND", interp, line, col);
	if (argc >= 5) EXPECT_STR_AT(args, 4, "UDP_SEND", interp, line, col);

	int hid = (int)as_i64(args[0]);
	const char* host = as_cstr(args[1]);
	int64_t port = as_i64(args[2]);
	const char* payload = as_cstr(args[3]);
	if (port < 0 || port > 65535) RUNTIME_ERROR(interp, "UDP_SEND: port out of range", line, col);

	SOCKET s = find_handle(g_state.udp, g_state.udp_count, hid);
	if (s == INVALID_SOCKET) RUNTIME_ERROR(interp, "UDP_SEND: invalid handle", line, col);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		RUNTIME_ERROR(interp, "UDP_SEND failed: invalid host", line, col);
	}

	int n = sendto(s, payload, (int)strlen(payload), 0, (const struct sockaddr*)&addr, sizeof(addr));
	if (n == SOCKET_ERROR) RUNTIME_ERROR(interp, "UDP_SEND failed", line, col);
	return value_int(n);
}

static Value op_udp_recv_text(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("UDP_RECV_TEXT", 2, 4);
	EXPECT_INT_AT(args, 0, "UDP_RECV_TEXT", interp, line, col);
	EXPECT_INT_AT(args, 1, "UDP_RECV_TEXT", interp, line, col);
	if (argc >= 3) EXPECT_INT_AT(args, 2, "UDP_RECV_TEXT", interp, line, col);
	if (argc >= 4) EXPECT_STR_AT(args, 3, "UDP_RECV_TEXT", interp, line, col);

	int hid = (int)as_i64(args[0]);
	int64_t max_bytes = as_i64(args[1]);
	int64_t timeout_ms = (argc >= 3) ? as_i64(args[2]) : 0;
	if (max_bytes <= 0 || max_bytes > 16 * 1024 * 1024) RUNTIME_ERROR(interp, "UDP_RECV_TEXT: max_bytes must be > 0", line, col);

	SOCKET s = find_handle(g_state.udp, g_state.udp_count, hid);
	if (s == INVALID_SOCKET) RUNTIME_ERROR(interp, "UDP_RECV_TEXT: invalid handle", line, col);
	set_socket_timeout_ms(s, ms_to_timeout_ms(timeout_ms));

	char* buf = (char*)malloc((size_t)max_bytes + 1);
	if (!buf) RUNTIME_ERROR(interp, "UDP_RECV_TEXT failed: out of memory", line, col);
	struct sockaddr_in from;
#ifdef _WIN32
	int from_len = (int)sizeof(from);
#else
	socklen_t from_len = sizeof(from);
#endif
	int n = recvfrom(s, buf, (int)max_bytes, 0, (struct sockaddr*)&from, &from_len);
	if (n == SOCKET_ERROR) {
		free(buf);
		RUNTIME_ERROR(interp, "UDP_RECV_TEXT failed", line, col);
	}
	buf[n < 0 ? 0 : n] = '\0';
	Value out = value_str(buf);
	free(buf);
	return out;
}

static Value op_udp_recv_bytes(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("UDP_RECV_BYTES", 2, 3);
	EXPECT_INT_AT(args, 0, "UDP_RECV_BYTES", interp, line, col);
	EXPECT_INT_AT(args, 1, "UDP_RECV_BYTES", interp, line, col);
	if (argc >= 3) EXPECT_INT_AT(args, 2, "UDP_RECV_BYTES", interp, line, col);

	int hid = (int)as_i64(args[0]);
	int64_t max_bytes = as_i64(args[1]);
	int64_t timeout_ms = (argc >= 3) ? as_i64(args[2]) : 0;
	if (max_bytes <= 0 || max_bytes > 16 * 1024 * 1024) RUNTIME_ERROR(interp, "UDP_RECV_BYTES: max_bytes must be > 0", line, col);

	SOCKET s = find_handle(g_state.udp, g_state.udp_count, hid);
	if (s == INVALID_SOCKET) RUNTIME_ERROR(interp, "UDP_RECV_BYTES: invalid handle", line, col);
	set_socket_timeout_ms(s, ms_to_timeout_ms(timeout_ms));

	unsigned char* buf = (unsigned char*)malloc((size_t)max_bytes);
	if (!buf) RUNTIME_ERROR(interp, "UDP_RECV_BYTES failed: out of memory", line, col);
	struct sockaddr_in from;
#ifdef _WIN32
	int from_len = (int)sizeof(from);
#else
	socklen_t from_len = sizeof(from);
#endif
	int n = recvfrom(s, (char*)buf, (int)max_bytes, 0, (struct sockaddr*)&from, &from_len);
	if (n == SOCKET_ERROR) {
		free(buf);
		RUNTIME_ERROR(interp, "UDP_RECV_BYTES failed", line, col);
	}
	Value out = bytes_to_tns(buf, (size_t)((n < 0) ? 0 : n));
	free(buf);
	if (out.type == VAL_NULL) RUNTIME_ERROR(interp, "UDP_RECV_BYTES failed: allocation", line, col);
	return out;
}

static Value op_udp_close(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("UDP_CLOSE", 1, 1);
	EXPECT_INT_AT(args, 0, "UDP_CLOSE", interp, line, col);
	int hid = (int)as_i64(args[0]);
	SOCKET s = INVALID_SOCKET;
	if (remove_handle(g_state.udp, &g_state.udp_count, hid, &s) != 0) {
		RUNTIME_ERROR(interp, "UDP_CLOSE: invalid handle", line, col);
	}
	closesocket(s);
	return value_int(0);
}

static Value op_http_get_text(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("HTTP_GET_TEXT", 1, 3);
	EXPECT_STR_AT(args, 0, "HTTP_GET_TEXT", interp, line, col);
	if (argc >= 2) EXPECT_INT_AT(args, 1, "HTTP_GET_TEXT", interp, line, col);
	if (argc >= 3) EXPECT_INT_AT(args, 2, "HTTP_GET_TEXT", interp, line, col);

#ifndef _WIN32
	RUNTIME_ERROR(interp, "HTTP_GET_TEXT not supported on this platform", line, col);
#else
	const char* url = as_cstr(args[0]);
	int timeout_ms = (argc >= 2) ? (int)as_i64(args[1]) : 5000;
	int verify = (argc >= 3) ? (int)as_i64(args[2]) : 1;
	int status = 0;
	unsigned char* body = NULL;
	size_t blen = 0;
	if (winhttp_request("GET", url, NULL, 0, NULL, timeout_ms, verify != 0, &status, &body, &blen) != 0) {
		RUNTIME_ERROR(interp, "HTTP_GET_TEXT failed", line, col);
	}
	(void)status;
	char* txt = (char*)malloc(blen + 1);
	if (!txt) {
		free(body);
		RUNTIME_ERROR(interp, "HTTP_GET_TEXT failed: out of memory", line, col);
	}
	memcpy(txt, body, blen);
	txt[blen] = '\0';
	free(body);
	Value out = value_str(txt);
	free(txt);
	return out;
#endif
}

static Value op_http_get_bytes(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("HTTP_GET_BYTES", 1, 3);
	EXPECT_STR_AT(args, 0, "HTTP_GET_BYTES", interp, line, col);
	if (argc >= 2) EXPECT_INT_AT(args, 1, "HTTP_GET_BYTES", interp, line, col);
	if (argc >= 3) EXPECT_INT_AT(args, 2, "HTTP_GET_BYTES", interp, line, col);

#ifndef _WIN32
	RUNTIME_ERROR(interp, "HTTP_GET_BYTES not supported on this platform", line, col);
#else
	const char* url = as_cstr(args[0]);
	int timeout_ms = (argc >= 2) ? (int)as_i64(args[1]) : 5000;
	int verify = (argc >= 3) ? (int)as_i64(args[2]) : 1;
	int status = 0;
	unsigned char* body = NULL;
	size_t blen = 0;
	if (winhttp_request("GET", url, NULL, 0, NULL, timeout_ms, verify != 0, &status, &body, &blen) != 0) {
		RUNTIME_ERROR(interp, "HTTP_GET_BYTES failed", line, col);
	}
	Value out = bytes_to_tns(body, blen);
	free(body);
	if (out.type == VAL_NULL) RUNTIME_ERROR(interp, "HTTP_GET_BYTES failed: allocation", line, col);
	return out;
#endif
}

static Value op_http_get_status(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("HTTP_GET_STATUS", 1, 3);
	EXPECT_STR_AT(args, 0, "HTTP_GET_STATUS", interp, line, col);
	if (argc >= 2) EXPECT_INT_AT(args, 1, "HTTP_GET_STATUS", interp, line, col);
	if (argc >= 3) EXPECT_INT_AT(args, 2, "HTTP_GET_STATUS", interp, line, col);

#ifndef _WIN32
	RUNTIME_ERROR(interp, "HTTP_GET_STATUS not supported on this platform", line, col);
#else
	const char* url = as_cstr(args[0]);
	int timeout_ms = (argc >= 2) ? (int)as_i64(args[1]) : 5000;
	int verify = (argc >= 3) ? (int)as_i64(args[2]) : 1;
	int status = 0;
	unsigned char* body = NULL;
	size_t blen = 0;
	if (winhttp_request("GET", url, NULL, 0, NULL, timeout_ms, verify != 0, &status, &body, &blen) != 0) {
		RUNTIME_ERROR(interp, "HTTP_GET_STATUS failed", line, col);
	}
	free(body);
	return value_int(status);
#endif
}

static Value op_http_post_text(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("HTTP_POST_TEXT", 2, 5);
	EXPECT_STR_AT(args, 0, "HTTP_POST_TEXT", interp, line, col);
	EXPECT_STR_AT(args, 1, "HTTP_POST_TEXT", interp, line, col);
	if (argc >= 3) EXPECT_STR_AT(args, 2, "HTTP_POST_TEXT", interp, line, col);
	if (argc >= 4) EXPECT_INT_AT(args, 3, "HTTP_POST_TEXT", interp, line, col);
	if (argc >= 5) EXPECT_INT_AT(args, 4, "HTTP_POST_TEXT", interp, line, col);

#ifndef _WIN32
	RUNTIME_ERROR(interp, "HTTP_POST_TEXT not supported on this platform", line, col);
#else
	const char* url = as_cstr(args[0]);
	const char* body_txt = as_cstr(args[1]);
	const char* content_type = (argc >= 3) ? as_cstr(args[2]) : "text/plain; charset=utf-8";
	int timeout_ms = (argc >= 4) ? (int)as_i64(args[3]) : 5000;
	int verify = (argc >= 5) ? (int)as_i64(args[4]) : 1;

	int status = 0;
	unsigned char* resp = NULL;
	size_t rlen = 0;
	if (winhttp_request("POST", url,
						(const unsigned char*)body_txt, strlen(body_txt),
						content_type, timeout_ms, verify != 0,
						&status, &resp, &rlen) != 0) {
		RUNTIME_ERROR(interp, "HTTP_POST_TEXT failed", line, col);
	}
	(void)status;
	char* txt = (char*)malloc(rlen + 1);
	if (!txt) {
		free(resp);
		RUNTIME_ERROR(interp, "HTTP_POST_TEXT failed: out of memory", line, col);
	}
	memcpy(txt, resp, rlen);
	txt[rlen] = '\0';
	free(resp);
	Value out = value_str(txt);
	free(txt);
	return out;
#endif
}

static Value op_ftp_list(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("FTP_LIST", 5, 8);
	EXPECT_STR_AT(args, 0, "FTP_LIST", interp, line, col);
	EXPECT_INT_AT(args, 1, "FTP_LIST", interp, line, col);
	EXPECT_STR_AT(args, 2, "FTP_LIST", interp, line, col);
	EXPECT_STR_AT(args, 3, "FTP_LIST", interp, line, col);
	EXPECT_STR_AT(args, 4, "FTP_LIST", interp, line, col);
	if (argc >= 6) EXPECT_INT_AT(args, 5, "FTP_LIST", interp, line, col);
	if (argc >= 7) EXPECT_INT_AT(args, 6, "FTP_LIST", interp, line, col);
	if (argc >= 8) EXPECT_INT_AT(args, 7, "FTP_LIST", interp, line, col);

	int64_t port = as_i64(args[1]);
	if (port < 0 || port > 65535) RUNTIME_ERROR(interp, "FTP_LIST: port out of range", line, col);

	char port_buf[32], tls_buf[32], timeout_buf[32], verify_buf[32];
	snprintf(port_buf, sizeof(port_buf), "%lld", (long long)port);
	snprintf(tls_buf, sizeof(tls_buf), "%lld", (long long)((argc >= 6) ? as_i64(args[5]) : 0));
	snprintf(timeout_buf, sizeof(timeout_buf), "%lld", (long long)((argc >= 7) ? as_i64(args[6]) : 10000));
	snprintf(verify_buf, sizeof(verify_buf), "%lld", (long long)((argc >= 8) ? as_i64(args[7]) : 1));

	const char* bargs[8] = { as_cstr(args[0]), port_buf, as_cstr(args[2]), as_cstr(args[3]), as_cstr(args[4]), tls_buf, timeout_buf, verify_buf };
	unsigned char* out = NULL;
	size_t out_len = 0;
	char* err = NULL;
	if (bridge_call("FTP_LIST", bargs, 8, &out, &out_len, &err) != 0) {
		char msg[512];
		snprintf(msg, sizeof(msg), "FTP_LIST failed: %s", err ? err : "bridge error");
		free(err);
		RUNTIME_ERROR(interp, msg, line, col);
	}
	char* txt = (char*)malloc(out_len + 1);
	if (!txt) {
		free(out);
		RUNTIME_ERROR(interp, "FTP_LIST failed: out of memory", line, col);
	}
	memcpy(txt, out, out_len);
	txt[out_len] = '\0';
	free(out);
	Value v = value_str(txt);
	free(txt);
	return v;
}

static Value op_ftp_get_bytes(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("FTP_GET_BYTES", 5, 8);
	EXPECT_STR_AT(args, 0, "FTP_GET_BYTES", interp, line, col);
	EXPECT_INT_AT(args, 1, "FTP_GET_BYTES", interp, line, col);
	EXPECT_STR_AT(args, 2, "FTP_GET_BYTES", interp, line, col);
	EXPECT_STR_AT(args, 3, "FTP_GET_BYTES", interp, line, col);
	EXPECT_STR_AT(args, 4, "FTP_GET_BYTES", interp, line, col);
	if (argc >= 6) EXPECT_INT_AT(args, 5, "FTP_GET_BYTES", interp, line, col);
	if (argc >= 7) EXPECT_INT_AT(args, 6, "FTP_GET_BYTES", interp, line, col);
	if (argc >= 8) EXPECT_INT_AT(args, 7, "FTP_GET_BYTES", interp, line, col);

	int64_t port = as_i64(args[1]);
	if (port < 0 || port > 65535) RUNTIME_ERROR(interp, "FTP_GET_BYTES: port out of range", line, col);

	char port_buf[32], tls_buf[32], timeout_buf[32], verify_buf[32];
	snprintf(port_buf, sizeof(port_buf), "%lld", (long long)port);
	snprintf(tls_buf, sizeof(tls_buf), "%lld", (long long)((argc >= 6) ? as_i64(args[5]) : 0));
	snprintf(timeout_buf, sizeof(timeout_buf), "%lld", (long long)((argc >= 7) ? as_i64(args[6]) : 10000));
	snprintf(verify_buf, sizeof(verify_buf), "%lld", (long long)((argc >= 8) ? as_i64(args[7]) : 1));

	const char* bargs[8] = { as_cstr(args[0]), port_buf, as_cstr(args[2]), as_cstr(args[3]), as_cstr(args[4]), tls_buf, timeout_buf, verify_buf };
	unsigned char* out = NULL;
	size_t out_len = 0;
	char* err = NULL;
	if (bridge_call("FTP_GET_BYTES", bargs, 8, &out, &out_len, &err) != 0) {
		char msg[512];
		snprintf(msg, sizeof(msg), "FTP_GET_BYTES failed: %s", err ? err : "bridge error");
		free(err);
		RUNTIME_ERROR(interp, msg, line, col);
	}
	Value v = bytes_to_tns(out, out_len);
	free(out);
	if (v.type == VAL_NULL) RUNTIME_ERROR(interp, "FTP_GET_BYTES failed: allocation", line, col);
	return v;
}

static Value op_ftp_put_bytes(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("FTP_PUT_BYTES", 6, 9);
	EXPECT_STR_AT(args, 0, "FTP_PUT_BYTES", interp, line, col);
	EXPECT_INT_AT(args, 1, "FTP_PUT_BYTES", interp, line, col);
	EXPECT_STR_AT(args, 2, "FTP_PUT_BYTES", interp, line, col);
	EXPECT_STR_AT(args, 3, "FTP_PUT_BYTES", interp, line, col);
	EXPECT_STR_AT(args, 4, "FTP_PUT_BYTES", interp, line, col);
	if (argc >= 7) EXPECT_INT_AT(args, 6, "FTP_PUT_BYTES", interp, line, col);
	if (argc >= 8) EXPECT_INT_AT(args, 7, "FTP_PUT_BYTES", interp, line, col);
	if (argc >= 9) EXPECT_INT_AT(args, 8, "FTP_PUT_BYTES", interp, line, col);

	int64_t port = as_i64(args[1]);
	if (port < 0 || port > 65535) RUNTIME_ERROR(interp, "FTP_PUT_BYTES: port out of range", line, col);
	unsigned char* payload = NULL;
	size_t payload_len = 0;
	if (tns_to_bytes(args[5], &payload, &payload_len) != 0) {
		RUNTIME_ERROR(interp, "FTP_PUT_BYTES expects TNS byte array", line, col);
	}
	char* payload_hex = hex_encode(payload, payload_len);
	free(payload);
	if (!payload_hex) RUNTIME_ERROR(interp, "FTP_PUT_BYTES failed: out of memory", line, col);

	char port_buf[32], tls_buf[32], timeout_buf[32], verify_buf[32];
	snprintf(port_buf, sizeof(port_buf), "%lld", (long long)port);
	snprintf(tls_buf, sizeof(tls_buf), "%lld", (long long)((argc >= 7) ? as_i64(args[6]) : 0));
	snprintf(timeout_buf, sizeof(timeout_buf), "%lld", (long long)((argc >= 8) ? as_i64(args[7]) : 10000));
	snprintf(verify_buf, sizeof(verify_buf), "%lld", (long long)((argc >= 9) ? as_i64(args[8]) : 1));

	const char* bargs[9] = { as_cstr(args[0]), port_buf, as_cstr(args[2]), as_cstr(args[3]), as_cstr(args[4]), payload_hex, tls_buf, timeout_buf, verify_buf };
	unsigned char* out = NULL;
	size_t out_len = 0;
	char* err = NULL;
	int rc = bridge_call("FTP_PUT_BYTES", bargs, 9, &out, &out_len, &err);
	free(payload_hex);
	if (rc != 0) {
		char msg[512];
		snprintf(msg, sizeof(msg), "FTP_PUT_BYTES failed: %s", err ? err : "bridge error");
		free(err);
		RUNTIME_ERROR(interp, msg, line, col);
	}
	free(out);
	return value_int(1);
}

static Value op_smtp_send(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes; (void)env;
	EXPECT_ARGC_MINMAX("SMTP_SEND", 8, 11);
	EXPECT_STR_AT(args, 0, "SMTP_SEND", interp, line, col);
	EXPECT_INT_AT(args, 1, "SMTP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 2, "SMTP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 3, "SMTP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 4, "SMTP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 5, "SMTP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 6, "SMTP_SEND", interp, line, col);
	EXPECT_STR_AT(args, 7, "SMTP_SEND", interp, line, col);
	if (argc >= 9) EXPECT_INT_AT(args, 8, "SMTP_SEND", interp, line, col);
	if (argc >= 10) EXPECT_INT_AT(args, 9, "SMTP_SEND", interp, line, col);
	if (argc >= 11) EXPECT_INT_AT(args, 10, "SMTP_SEND", interp, line, col);

	int64_t port = as_i64(args[1]);
	if (port < 0 || port > 65535) RUNTIME_ERROR(interp, "SMTP_SEND: port out of range", line, col);

	char port_buf[32], tls_buf[32], timeout_buf[32], verify_buf[32];
	snprintf(port_buf, sizeof(port_buf), "%lld", (long long)port);
	snprintf(tls_buf, sizeof(tls_buf), "%lld", (long long)((argc >= 9) ? as_i64(args[8]) : 1));
	snprintf(timeout_buf, sizeof(timeout_buf), "%lld", (long long)((argc >= 10) ? as_i64(args[9]) : 10000));
	snprintf(verify_buf, sizeof(verify_buf), "%lld", (long long)((argc >= 11) ? as_i64(args[10]) : 1));

	const char* bargs[11] = {
		as_cstr(args[0]), port_buf, as_cstr(args[2]), as_cstr(args[3]), as_cstr(args[4]),
		as_cstr(args[5]), as_cstr(args[6]), as_cstr(args[7]), tls_buf, timeout_buf, verify_buf
	};
	unsigned char* out = NULL;
	size_t out_len = 0;
	char* err = NULL;
	if (bridge_call("SMTP_SEND", bargs, 11, &out, &out_len, &err) != 0) {
		char msg[512];
		snprintf(msg, sizeof(msg), "SMTP_SEND failed: %s", err ? err : "bridge error");
		free(err);
		RUNTIME_ERROR(interp, msg, line, col);
	}
	free(out);
	return value_int(1);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void prefix_extension_init(prefix_ext_context* ctx) {
	if (!ctx) return;
	ensure_socket_runtime();
	ctx->register_operator("TCP_CONNECT", op_tcp_connect, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("TCP_SEND", op_tcp_send, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("TCP_RECV_TEXT", op_tcp_recv_text, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("TCP_RECV_BYTES", op_tcp_recv_bytes, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("TCP_CLOSE", op_tcp_close, PREFIX_EXTENSION_ASMODULE);

	ctx->register_operator("UDP_BIND", op_udp_bind, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("UDP_SEND", op_udp_send, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("UDP_RECV_TEXT", op_udp_recv_text, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("UDP_RECV_BYTES", op_udp_recv_bytes, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("UDP_CLOSE", op_udp_close, PREFIX_EXTENSION_ASMODULE);

	ctx->register_operator("HTTP_GET_TEXT", op_http_get_text, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("HTTP_GET_BYTES", op_http_get_bytes, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("HTTP_GET_STATUS", op_http_get_status, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("HTTP_POST_TEXT", op_http_post_text, PREFIX_EXTENSION_ASMODULE);

	ctx->register_operator("FTP_LIST", op_ftp_list, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("FTP_GET_BYTES", op_ftp_get_bytes, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("FTP_PUT_BYTES", op_ftp_put_bytes, PREFIX_EXTENSION_ASMODULE);

	ctx->register_operator("SMTP_SEND", op_smtp_send, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("MTP_SEND", op_smtp_send, PREFIX_EXTENSION_ASMODULE);
}