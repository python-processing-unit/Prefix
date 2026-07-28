/* Centralized markdown renderer for the docs.
   Loads marked.js if necessary, then renders the markdown embedded
   in the page's `#md` element into `#content` and normalizes heading ids. */
(function(){
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
    style.textContent = '\n' +
      '.generated-toc { padding-left: 1.2rem; }\n' +
      '.generated-toc > li { margin: 0.6rem 0; }\n' +
      '.generated-toc li ul { margin-top: 0.25rem; margin-bottom: 0.25rem; }\n';
    document.head.appendChild(style);
  }

  function findTocHeading(root) {
    return Array.from(root.querySelectorAll('h2')).find(function(h){
      return (h.textContent || '').trim().toLowerCase() === 'table of contents';
    });
  }

  function buildTocList(root, tocHeading) {
    var tocLevels = new Set([2,3,4]);
    var items = Array.from(root.querySelectorAll('h2,h3,h4')).filter(function(h){ return h !== tocHeading; });
    if (items.length === 0) return null;

    var tocRoot = document.createElement('ul');
    tocRoot.className = 'generated-toc';
    var listStack = [tocRoot];
    var currentLevel = 2;

    items.forEach(function(h){
      var level = Number(h.tagName.slice(1));
      if (!tocLevels.has(level)) return;

      while (currentLevel < level) {
        var parentList = listStack[listStack.length - 1];
        var parentLi = parentList.lastElementChild;
        if (!parentLi || parentLi.tagName.toLowerCase() !== 'li') {
          parentLi = document.createElement('li');
          parentLi.textContent = '';
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
      a.href = '#' + h.id;
      a.textContent = (h.textContent || '').trim();
      li.appendChild(a);
      listStack[listStack.length - 1].appendChild(li);
    });

    return tocRoot;
  }

  function replaceTocBlock(root) {
    var tocHeading = findTocHeading(root);
    if (!tocHeading) return;

    var stop = tocHeading.nextElementSibling;
    while (stop && stop.tagName.toLowerCase() !== 'hr') {
      stop = stop.nextElementSibling;
    }

    var node = tocHeading.nextSibling;
    while (node && node !== stop) {
      var next = node.nextSibling;
      node.remove();
      node = next;
    }

    ensureTocStyle();
    var tocList = buildTocList(root, tocHeading);
    if (!tocList) return;

    if (stop) {
      root.insertBefore(tocList, stop);
    } else {
      root.appendChild(tocList);
    }
  }

  function render() {
    var mdEl = document.getElementById('md');
    if (!mdEl) return;
    var md = mdEl.textContent;
    var rendered = (window.marked && typeof marked.parse === 'function') ? marked.parse(md) : md;
    var container = document.getElementById('content');
    if (container) container.innerHTML = rendered;

    var headings = container ? container.querySelectorAll('h1,h2,h3,h4,h5,h6') : [];
    Array.prototype.forEach.call(headings, function(h){
      if (!h.id || h.id.trim() === '') {
        h.id = slugify(h.textContent || h.innerText || '');
      }
    });

    replaceTocBlock(container);
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

  document.addEventListener('DOMContentLoaded', function(){
    ensureMarked(render);
  });
})();
