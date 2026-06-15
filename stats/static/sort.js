// Client-side table sorting for the public stats site.
//
// CSP-safe (external file, no inline JS). Any <table class="sortable"> gets
// clickable column headers: click to sort, click again to reverse. Numeric
// columns (headers tagged .num or .rank) sort numerically; everything else
// sorts as text (case-insensitive, natural order). The data is already fully
// rendered server-side, so this is purely a view convenience -- no fetch.
//
// The "#" / rank column is left as-is (it's the server-side pick rank); sorting
// reorders the rows but each row keeps its rank number, and clicking the "#"
// header restores the original pick order.
(function () {
  'use strict';

  function cellText(row, idx) {
    var cell = row.children[idx];
    return cell ? cell.textContent.trim() : '';
  }

  // Pull a comparable number out of a cell ("12", "57.3%", "1,024", "—").
  // Non-numeric / empty cells sort to the bottom regardless of direction-
  // friendly defaults by returning -Infinity (they fall last on descending,
  // which is what you want for "no winrate yet" rows).
  function asNumber(text) {
    if (text == null) return -Infinity;
    var cleaned = text.replace(/[^0-9.\-]/g, '');
    if (cleaned === '' || cleaned === '-' || cleaned === '.') return -Infinity;
    var n = parseFloat(cleaned);
    return isNaN(n) ? -Infinity : n;
  }

  function sortBody(table, idx, numeric, asc) {
    var body = table.tBodies[0];
    if (!body) return;
    var rows = Array.prototype.slice.call(body.rows);
    rows.sort(function (a, b) {
      var av = cellText(a, idx), bv = cellText(b, idx);
      var cmp = numeric
        ? asNumber(av) - asNumber(bv)
        : av.localeCompare(bv, undefined, { numeric: true, sensitivity: 'base' });
      if (cmp === 0) return 0;
      return asc ? cmp : -cmp;
    });
    // Re-append in sorted order (moves existing nodes, no rebuild).
    var frag = document.createDocumentFragment();
    rows.forEach(function (r) { frag.appendChild(r); });
    body.appendChild(frag);
  }

  function wire(table) {
    var head = table.tHead;
    if (!head || !head.rows[0]) return;
    var headers = head.rows[0].children;

    Array.prototype.forEach.call(headers, function (th, idx) {
      var numeric = th.classList.contains('num') || th.classList.contains('rank');
      th.classList.add('sort-th');
      th.setAttribute('role', 'button');
      th.setAttribute('tabindex', '0');
      th.setAttribute('aria-label', 'Sort by ' + th.textContent.trim());

      function activate() {
        var dir = th.getAttribute('data-sort-dir');
        // Same column: flip. Fresh column: numeric defaults high->low (desc),
        // text defaults A->Z (asc) -- the intuitive first click for each.
        var asc = dir === 'desc' ? true : (dir === 'asc' ? false : !numeric);

        Array.prototype.forEach.call(headers, function (o) {
          o.removeAttribute('data-sort-dir');
          o.classList.remove('sort-asc', 'sort-desc');
        });
        th.setAttribute('data-sort-dir', asc ? 'asc' : 'desc');
        th.classList.add(asc ? 'sort-asc' : 'sort-desc');

        sortBody(table, idx, numeric, asc);
      }

      th.addEventListener('click', activate);
      th.addEventListener('keydown', function (e) {
        if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar') {
          e.preventDefault();
          activate();
        }
      });
    });
  }

  document.addEventListener('DOMContentLoaded', function () {
    var tables = document.querySelectorAll('table.sortable');
    Array.prototype.forEach.call(tables, wire);
  });
})();
