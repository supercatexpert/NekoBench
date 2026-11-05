/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
(function () {
  'use strict';
  var NS = 'http://www.w3.org/2000/svg';
  var root = document.documentElement;

  /* ---------- theme toggle ---------- */
  var themeBtn = document.getElementById('nb-theme');
  if (themeBtn) {
    themeBtn.addEventListener('click', function () {
      var cur = root.getAttribute('data-theme');
      var dark = cur ? cur === 'dark' : window.matchMedia('(prefers-color-scheme: dark)').matches;
      root.setAttribute('data-theme', dark ? 'light' : 'dark');
    });
  }

  /* ---------- helpers ---------- */
  function svgEl(tag, attrs, parent) {
    var e = document.createElementNS(NS, tag);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }
  function div(cls, text, parent) {
    var e = document.createElement(cls === 'b' || cls === 'span' ? cls : 'div');
    if (cls !== 'b' && cls !== 'span' && cls) e.className = cls;
    if (text != null) e.textContent = text;
    if (parent) parent.appendChild(e);
    return e;
  }
  function trim(v, d) { var s = v.toFixed(d); return s.indexOf('.') >= 0 ? s.replace(/0+$/, '').replace(/[.]$/, '') : s; }
  function fmtSize(b) {
    var u = ['B', 'KiB', 'MiB', 'GiB', 'TiB'], i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; i++; }
    return trim(b, 1) + ' ' + u[i];
  }
  function shortSize(b) {
    var u = ['B', 'K', 'M', 'G', 'T'], i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; i++; }
    return trim(b, 1) + u[i];
  }
  function fmtNum(v) { return v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2); }
  function fmtTick(v) { return v >= 1000 ? trim(v / 1000, 1) + 'k' : trim(v, 2); }

  /* one shared tooltip */
  var tip = div('tip', null, document.body);
  function showTip(x, y) {
    tip.style.display = 'block';
    var w = tip.offsetWidth, h = tip.offsetHeight;
    var left = x + 14, top = y + 14;
    if (left + w > window.innerWidth - 8) left = x - w - 14;
    if (top + h > window.innerHeight - 8) top = y - h - 14;
    tip.style.left = Math.max(8, left) + 'px';
    tip.style.top = Math.max(8, top) + 'px';
  }
  function hideTip() { tip.style.display = 'none'; }
  function tipRow(value, label, slot) {
    var r = div('row', null, tip);
    if (slot) div('key bg' + slot, null, r);
    div('b', value, r);
    var l = div('span', label, r);
    l.className = 'lbl';
  }

  /* ---------- log-log line chart with crosshair ---------- */
  function lineChart(host, cfg) {
    var xs = cfg.xs;
    if (!xs || xs.length < 2) return;
    var W = 900, H = 330, ml = 56, mr = 132, mt = 58, mb = 42;
    var pw = W - ml - mr, ph = H - mt - mb;
    var lo = Infinity, hi = 0;
    cfg.series.forEach(function (s) {
      s.ys.forEach(function (v) { if (v != null && v > 0) { lo = Math.min(lo, v); hi = Math.max(hi, v); } });
    });
    if (!isFinite(lo)) return;
    var x0 = Math.log2(xs[0]), x1 = Math.log2(xs[xs.length - 1]);
    var y0 = Math.log10(lo / 1.25), y1 = Math.log10(hi * 1.25);
    function X(v) { return ml + (Math.log2(v) - x0) / (x1 - x0) * pw; }
    function Y(v) { return mt + ph - (Math.log10(v) - y0) / (y1 - y0) * ph; }

    var svg = svgEl('svg', { viewBox: '0 0 ' + W + ' ' + H, 'class': 'chart', role: 'img',
                             tabindex: '0', 'aria-label': cfg.title }, host);

    /* y grid: 1-2-5 ticks */
    for (var e = Math.floor(y0); e <= Math.ceil(y1); e++) {
      [1, 2, 5].forEach(function (m) {
        var v = m * Math.pow(10, e), lv = Math.log10(v);
        if (lv < y0 || lv > y1) return;
        var y = Y(v);
        svgEl('line', { x1: ml, x2: ml + pw, y1: y, y2: y, 'class': 'grid' }, svg);
        svgEl('text', { x: ml - 8, y: y + 4, 'class': 'tick', 'text-anchor': 'end' }, svg).textContent = fmtTick(v);
      });
    }
    svgEl('line', { x1: ml, x2: ml + pw, y1: mt + ph, y2: mt + ph, 'class': 'axis' }, svg);
    /* x ticks at powers of 4 */
    for (var k = Math.ceil(x0 / 2) * 2; k <= x1; k += 2) {
      var xv = Math.pow(2, k), xx = X(xv);
      svgEl('line', { x1: xx, x2: xx, y1: mt + ph, y2: mt + ph + 4, 'class': 'axis' }, svg);
      svgEl('text', { x: xx, y: mt + ph + 17, 'class': 'tick', 'text-anchor': 'middle' }, svg).textContent = shortSize(xv);
    }
    svgEl('text', { x: ml + pw / 2, y: H - 6, 'class': 'title', 'text-anchor': 'middle' }, svg).textContent = 'Working set (log scale)';
    svgEl('text', { x: ml, y: 14, 'class': 'title' }, svg).textContent = cfg.yTitle;

    /* cache-size markers, labels staggered into rows to avoid collisions */
    var rowsEnd = [-1e9, -1e9, -1e9];
    (cfg.markers || []).slice().sort(function (a, b) { return a.x - b.x; }).forEach(function (m) {
      if (m.x < xs[0] || m.x > xs[xs.length - 1]) return;
      var x = X(m.x), w = m.label.length * 6.2 + 8, r = 0;
      while (r < rowsEnd.length - 1 && x - w / 2 < rowsEnd[r]) r++;
      rowsEnd[r] = x + w / 2;
      var ly = mt - 8 - r * 13;
      svgEl('line', { x1: x, x2: x, y1: ly + 3, y2: mt + ph, 'class': 'mk st' + m.slot }, svg);
      svgEl('text', { x: x, y: ly, 'class': 'mklabel', 'text-anchor': 'middle' }, svg).textContent = m.label;
    });

    /* series lines */
    cfg.series.forEach(function (s) {
      var d = '', pen = false;
      s.ys.forEach(function (v, i) {
        if (v == null || !(v > 0)) { pen = false; return; }
        d += (pen ? 'L' : 'M') + X(xs[i]).toFixed(1) + ' ' + Y(v).toFixed(1);
        pen = true;
      });
      svgEl('path', { d: d, 'class': 'ln st' + s.slot }, svg);
    });

    /* direct end labels, spread apart vertically */
    var ends = [];
    cfg.series.forEach(function (s) {
      for (var i = s.ys.length - 1; i >= 0; i--)
        if (s.ys[i] != null && s.ys[i] > 0) { ends.push({ y: Y(s.ys[i]), s: s }); break; }
    });
    ends.sort(function (a, b) { return a.y - b.y; });
    for (var i = 1; i < ends.length; i++) if (ends[i].y - ends[i - 1].y < 15) ends[i].y = ends[i - 1].y + 15;
    ends.forEach(function (e2) {
      svgEl('line', { x1: ml + pw + 6, x2: ml + pw + 18, y1: e2.y, y2: e2.y, 'class': 'ln st' + e2.s.slot }, svg);
      svgEl('text', { x: ml + pw + 22, y: e2.y + 4, 'class': 'endlabel' }, svg).textContent = e2.s.short;
    });

    /* hover layer */
    var xh = svgEl('line', { y1: mt, y2: mt + ph, 'class': 'xh', visibility: 'hidden' }, svg);
    var dots = cfg.series.map(function (s) {
      return svgEl('circle', { r: 4, 'class': 'dot fl' + s.slot, visibility: 'hidden' }, svg);
    });
    var hit = svgEl('rect', { x: ml, y: mt - 10, width: pw, height: ph + 10, fill: 'transparent' }, svg);
    var cur = -1;

    function show(idx, cx, cy) {
      cur = idx;
      var x = X(xs[idx]);
      xh.setAttribute('x1', x); xh.setAttribute('x2', x); xh.setAttribute('visibility', 'visible');
      tip.textContent = '';
      div('hd', fmtSize(xs[idx]), tip);
      cfg.series.forEach(function (s, si) {
        var v = s.ys[idx];
        if (v == null) { dots[si].setAttribute('visibility', 'hidden'); return; }
        dots[si].setAttribute('cx', x); dots[si].setAttribute('cy', Y(v));
        dots[si].setAttribute('visibility', 'visible');
        tipRow(fmtNum(v) + ' ' + cfg.unit, s.name, s.slot);
      });
      if (cx == null) {
        var bb = svg.getBoundingClientRect();
        cx = bb.left + x / W * bb.width; cy = bb.top + mt / H * bb.height;
      }
      showTip(cx, cy);
    }
    function clear() {
      cur = -1; hideTip(); xh.setAttribute('visibility', 'hidden');
      dots.forEach(function (d) { d.setAttribute('visibility', 'hidden'); });
    }
    function nearest(evt) {
      var bb = svg.getBoundingClientRect();
      var px = (evt.clientX - bb.left) / bb.width * W, best = 0, bd = Infinity;
      xs.forEach(function (v, i) { var dd = Math.abs(X(v) - px); if (dd < bd) { bd = dd; best = i; } });
      return best;
    }
    hit.addEventListener('pointermove', function (evt) { show(nearest(evt), evt.clientX, evt.clientY); });
    hit.addEventListener('pointerleave', clear);
    svg.addEventListener('blur', clear);
    svg.addEventListener('focus', function () { show(cur < 0 ? 0 : cur); });
    svg.addEventListener('keydown', function (evt) {
      if (evt.key === 'ArrowRight' || evt.key === 'ArrowLeft') {
        var n = cur < 0 ? 0 : cur + (evt.key === 'ArrowRight' ? 1 : -1);
        show(Math.max(0, Math.min(xs.length - 1, n)));
        evt.preventDefault();
      } else if (evt.key === 'Escape') { clear(); }
    });
  }

  /* ---------- data-driven charts ---------- */
  var dataEl = document.getElementById('nb-data');
  var D = dataEl ? JSON.parse(dataEl.textContent) : {};
  if (D.sweep && D.sweep.series.length) {
    var mk = function (field) {
      return D.sweep.series.map(function (s) { return { name: s.name, short: s.short, slot: s.slot, ys: s[field] }; });
    };
    var latHost = document.getElementById('chart-lat');
    if (latHost) lineChart(latHost, { title: 'Load-to-use latency versus working set', yTitle: 'Latency, ns (log scale)',
                                      unit: 'ns', xs: D.sweep.xs, series: mk('lat'), markers: D.sweep.markers });
    var bwHost = document.getElementById('chart-bw');
    if (bwHost) lineChart(bwHost, { title: 'Single-thread read bandwidth versus working set', yTitle: 'Read bandwidth, GB/s (log scale)',
                                    unit: 'GB/s', xs: D.sweep.xs, series: mk('bw'), markers: D.sweep.markers });
  }

  /* ---------- heatmap tooltips ---------- */
  var cats = D.c2cCats || [];
  Array.prototype.forEach.call(document.querySelectorAll('table.heat'), function (tbl) {
    var heads = tbl.querySelectorAll('thead th[data-cpu]');
    var marked = [];
    function unmark() { marked.forEach(function (h) { h.classList.remove('hl'); }); marked = []; }
    tbl.addEventListener('pointerover', function (evt) {
      var td = evt.target.closest ? evt.target.closest('td[data-v]') : null;
      unmark();
      if (!td) { hideTip(); return; }
      var a = td.getAttribute('data-a'), b = td.getAttribute('data-b');
      var rh = td.parentNode.querySelector('th');
      var col = Array.prototype.filter.call(heads, function (h) { return h.getAttribute('data-cpu') === b; })[0];
      [rh, col].forEach(function (h) { if (h) { h.classList.add('hl'); marked.push(h); } });
      tip.textContent = '';
      div('hd', 'CPU ' + a + ' ↔ CPU ' + b, tip);
      tipRow(td.getAttribute('data-v') + ' ns', 'one-way', 0);
      var c = cats[+td.getAttribute('data-k')];
      if (c) div('hd', c, tip).style.margin = '4px 0 0';
      var bb = td.getBoundingClientRect();
      showTip(bb.right, bb.bottom);
    });
    tbl.addEventListener('pointerleave', function () { unmark(); hideTip(); });
  });
})();
