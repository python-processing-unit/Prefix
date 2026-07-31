/* Centralized markdown renderer + fully auto-generated multi-file TOC.
   - Renders the markdown in #md into #content (using marked.js).
   - Assigns github-slugger-style unique ids to every heading.
   - Builds a Table of Contents covering EVERY spec file and ALL of their
     subheadings, straight from window.SPEC_INDEX (generated from source).
     Links belonging to the current file are marked .current (bold). */
(function () {
  'use strict';

  function slugify(text) {
    return (text || '')
      .trim()
      .toLowerCase()
      .replace(/<[^>]+>/g, '')
      .replace(/[^a-z0-9\s-]/g, '')
      .replace(/\s+/g, '-');
  }

  function ensureTocStyle() {
    if (document.getElementById('generated-toc-style')) return;
    var style = document.createElement('style');
    style.id = 'generated-toc-style';
    style.textContent =
      '\n' +
      '.generated-toc { padding-left: 1.2rem; }\n' +
      '.generated-toc > li { margin: 0.6rem 0; }\n' +
      '.generated-toc li ul { margin-top: 0.25rem; margin-bottom: 0.25rem; }\n' +
      '.generated-toc a.current { font-weight: 700; }\n' +
      '.toc-title { text-align: left; }\n';
    document.head.appendChild(style);
  }

  function render() {
    var mdEl = document.getElementById('md');
    if (!mdEl) return;
    var md = mdEl.textContent;
    var rendered =
      window.marked && typeof marked.parse === 'function' ? marked.parse(md) : md;
    var container = document.getElementById('content');
    if (container) container.innerHTML = rendered;

    var headings = container
      ? container.querySelectorAll('h1,h2,h3,h4,h5,h6')
      : [];
    var used = Object.create(null);
    Array.prototype.forEach.call(headings, function (h) {
      var slug = slugify(h.textContent || h.innerText || '');
      var cand = slug,
        n = 1;
      while (used[cand]) {
        cand = slug + '-' + n;
        n++;
      }
      used[cand] = true;
      h.id = cand;
    });

    buildMultiFileToc(container);
  }

  function buildMultiFileToc(container) {
    var index = window.SPEC_INDEX;
    if (!index || !index.length || !container) return;

    var cur = (location.pathname.split('/').pop() || location.href.split('/').pop() || '')
      .toLowerCase();

    ensureTocStyle();

    var nav = document.createElement('nav');
    nav.id = 'spec-toc';

    var title = document.createElement('h2');
    title.className = 'toc-title';
    title.textContent = 'Table of contents';
    nav.appendChild(title);

    var rootUl = document.createElement('ul');
    rootUl.className = 'generated-toc';

    // One continuous stack across every file so each file's top-level
    // heading is a sibling of the others (no empty wrapper <li>).
    var listStack = [rootUl];
    var currentLevel = 2;

    index.forEach(function (fileEntry) {
      (fileEntry.headings || []).forEach(function (h) {
        var level = h.level;
        while (currentLevel < level) {
          var parentList = listStack[listStack.length - 1];
          var parentLi = parentList.lastElementChild;
          if (!parentLi || parentLi.tagName.toLowerCase() !== 'li') {
            parentLi = document.createElement('li');
            parentList.appendChild(parentLi);
          }
          var nested = document.createElement('ul');
          parentLi.appendChild(nested);
          listStack.push(nested);
          currentLevel += 1;
        }
        while (currentLevel > level) {
          listStack.pop();
          currentLevel -= 1;
        }
        var li = document.createElement('li');
        var a = document.createElement('a');
        var sameFile = (fileEntry.file || '').toLowerCase() === cur;
        a.href = (sameFile ? '#' : fileEntry.file + '#') + h.slug;
        a.textContent = h.text;
        if (sameFile) a.className = 'current';
        li.appendChild(a);
        listStack[listStack.length - 1].appendChild(li);
      });
    });

    nav.appendChild(rootUl);

    var hr = document.createElement('hr');

    var h1 = container.querySelector('h1');
    var ref = h1 && h1.nextSibling ? h1.nextSibling : container.firstChild;
    container.insertBefore(hr, ref);
    container.insertBefore(nav, ref);
  }

  function ensureMarked(cb) {
    if (window.marked && typeof marked.parse === 'function') {
      cb();
      return;
    }
    var s = document.createElement('script');
    s.src = 'https://cdn.jsdelivr.net/npm/marked/marked.min.js';
    s.onload = cb;
    s.onerror = cb;
    document.head.appendChild(s);
  }

  document.addEventListener('DOMContentLoaded', function () {
    ensureMarked(render);
  });
})();
