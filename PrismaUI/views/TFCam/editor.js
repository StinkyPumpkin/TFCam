// TFCam Camera Path editor (PrismaUI view). TFCam pushes its state with tfcamState(json) about ten times a second while
// the editor is open; every change goes back through tfcamCmd("cmd|arg|..."), handled on the game's main thread.
// Outside the game (a normal browser) it shows demo data so the layout can be checked.
(function () {
  'use strict';

  var MOVES = ['straight', 'curve', 'around', 'cut'];
  var MOVE_NAMES = ['Straight', 'Curve', 'Around', 'Cut'];
  var LOOKS = ['The pivot', 'As I set it', 'Where it goes'];
  var WAYS = ['Shortest', 'Left', 'Right'];
  var BONES = ['Middle', 'Head', 'Eyes', 'Chest', 'Hips', 'Bum', 'Groin', 'Hands', 'Feet'];
  var PRESETS = [
    ['Steady', [0.33, 0.33, 0.67, 0.67]], ['Slow start and stop', [0.42, 0, 0.58, 1]], ['Slow start', [0.42, 0, 1, 1]],
    ['Slow stop', [0, 0, 0.58, 1]], ['Fast start, long glide', [0.1, 0.7, 0.3, 1]], ['Hold, then rush', [0.8, 0, 0.9, 0.3]]
  ];

  // camera options shown as sliders: the slider's own number <-> the camera's value
  var F = {
    wait: { min: 0, max: 10, step: 0.1, get: function (c) { return c.wait; }, put: function (v) { return v; }, text: function (v) { return v.toFixed(1) + ' s'; } },
    fov: { min: 10, max: 120, step: 1, get: function (c) { return c.fov; }, put: function (v) { return v; }, text: function (v) { return Math.round(v) + '°'; } },
    time: { min: 0.5, max: 30, step: 0.1, get: function (c) { return c.time; }, put: function (v) { return v; }, text: function (v) { return v.toFixed(1) + ' s'; } },
    bendSide: pct(-100, 100, 'bendSide', true), bendUp: pct(-100, 100, 'bendUp', true),
    turns: { min: 0, max: 10, step: 1, get: function (c) { return c.turns; }, put: function (v) { return Math.round(v); }, text: function (v) { return String(Math.round(v)); } },
    shake: pct(0, 100, 'shake'), elastic: pct(0, 100, 'elastic'), sling: pct(0, 100, 'sling'),
    slowAt: pct(0, 100, 'slowAt', false, '%'), slowLength: pct(5, 100, 'slowLength', false, '%'), blurStrength: pct(10, 100, 'blurStrength'),
    slowSpeed: { min: 0, max: 95, step: 1, get: function (c) { return Math.round((1 - c.slowSpeed) * 100); }, put: function (v) { return 1 - v / 100; }, text: function (v) { return Math.round(v) + '%'; } }
  };
  function pct(min, max, key, signed, unit) {
    return {
      min: min, max: max, step: 1,
      get: function (c) { return Math.round(c[key] * 100); },
      put: function (v) { return v / 100; },
      text: function (v) { return (signed && v > 0 ? '+' : '') + Math.round(v) + (unit || ''); }
    };
  }

  var S = null;           // the last state from TFCam
  var sel = 0;            // the selected camera
  var tab = 'move';
  var busy = false;       // a slider or the curve is being dragged: the inspector is not redrawn under it
  var scrubbing = false;
  var scrubAt = 0;
  var dragHandle = -1;
  var lastKey = '';
  var clearArmed = false;
  var preview = false;

  function $(id) { return document.getElementById(id); }
  function esc(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); }
  function cmd(s) {
    if (typeof window.tfcamCmd === 'function') window.tfcamCmd(s);
    else if (preview) previewCmd(s);
  }
  function clock(s) {
    s = Math.max(0, s || 0);
    var m = Math.floor(s / 60), r = s - m * 60;
    return m + ':' + (r < 10 ? '0' : '') + r.toFixed(1);
  }
  var throttles = {};
  function throttled(key, fn) {  // at most every 60 ms, and always the last one
    var t = throttles[key] || (throttles[key] = { at: 0, timer: 0, fn: null });
    t.fn = fn;
    var now = Date.now();
    if (now - t.at >= 60) { t.at = now; fn(); return; }
    if (!t.timer) t.timer = setTimeout(function () { t.timer = 0; t.at = Date.now(); t.fn(); }, 60);
  }

  // ---- the path's times ---------------------------------------------------------------------------------------------
  function hasNext(i) { var n = S.cams.length; return i < n - 1 || (S.loop && n > 1); }
  function layout() {
    var out = { segs: [], marks: [], total: 0 }, t = 0;
    S.cams.forEach(function (c, i) {
      out.marks.push({ i: i, t: t });
      if (c.wait > 0) out.segs.push({ kind: 'wait', t: t, d: c.wait, label: 'Wait ' + c.wait.toFixed(1) + ' s', cam: i });
      t += c.wait;
      if (hasNext(i)) {
        if (c.move === 3) {
          out.segs.push({ kind: 'cut', t: t, d: 0, label: 'Cut', cam: i });
        } else {
          var lbl = MOVE_NAMES[c.move] + ' ' + c.seconds.toFixed(1) + ' s';
          if (c.slowMo) lbl += c.slowCamera ? ' · slow motion' : ' · bullet time';
          if (c.elastic > 0) lbl += ' · elastic';
          if (c.sling > 0) lbl += ' · slingshot';
          if (c.crashZoom) lbl += ' · crash zoom';
          out.segs.push({ kind: MOVES[c.move], t: t, d: c.seconds, label: lbl, cam: i, slow: c.slowMo ? c : null });
          t += c.seconds;
        }
      }
    });
    out.total = t > 0.01 ? t : Math.max(S.total || 0, 0.01);  // the cameras' own times (TFCam's total is the same sum)
    return out;
  }
  function camAt(lay, t) {
    var at = { cam: 0, moving: false };
    for (var k = 0; k < lay.segs.length; k++) {
      var s = lay.segs[k];
      if (t >= s.t && t < s.t + Math.max(s.d, 0.001)) return { cam: s.cam, moving: s.kind !== 'wait' };
    }
    lay.marks.forEach(function (m) { if (t >= m.t) at = { cam: m.i, moving: false }; });
    return at;
  }

  // ---- drawing --------------------------------------------------------------------------------------------------------
  function render(force) {
    if (!S) return;
    var n = S.cams.length;
    if (sel >= n) sel = Math.max(0, n - 1);
    var key = JSON.stringify([S.cams, S.loop, sel, tab, S.atCam, S.playing]);
    renderTop();
    if (force || key !== lastKey) {
      lastKey = key;
      renderCams();
      renderTimeline();  // before the transport: the scrub's range first, then its value
      if (!busy) renderInspector();
    }
    renderTransport();
  }

  function renderTop() {
    setOn($('tgFollow'), S.follow);
    setOn($('tgLoop'), S.loop);
    setOn($('tgPath'), S.showPath);
    setOn($('btFreeze'), S.frozen);
    $('freezeLabel').textContent = S.frozen ? 'Unfreeze world' : 'Freeze world';
    setOn($('btClean'), S.clean);
    setOn($('btVideo'), S.video);
    $('videoLabel').textContent = S.video ? 'Stop video' : 'Record video';
    $('btVideo').title = S.videoStatus || '';
    var open = $('open');
    var names = (S.saved || []).join('\n') + '|' + S.name;
    if (open.dataset.names !== names) {
      open.dataset.names = names;
      var html = '<option value="">' + esc(S.name || 'Current') + '</option>';
      (S.saved || []).forEach(function (p) { html += '<option value="' + esc(p) + '">Open: ' + esc(p) + '</option>'; });
      open.innerHTML = html;
      open.value = '';
    }
    var notice = $('notice');
    var msg = S.recordingPath ? 'Recording a manual path - close the editor and fly; open it again to stop.'
      : (!S.freeCam ? 'The free camera is off - turn it on to add cameras and play the path.' : '');
    notice.hidden = !msg;
    notice.textContent = msg;
    $('btAdd').disabled = !S.freeCam || S.recordingPath;
    var rec = $('btRecordPath');
    rec.textContent = S.recordingPath ? 'Stop recording the path' : 'Record manual path (fly it)';
    var fix = $('btFix');
    fix.textContent = S.fixed ? 'Let the camera go' : 'Fix camera here';
    fix.dataset.cmd = S.fixed ? 'release' : 'fix';
    $('watch').checked = !!S.watching;
    if (!busy) {
      $('g-smooth').value = Math.round((S.smoothness || 0) * 100);
      $('g-aim').value = Math.round(S.aimHeight || 0);
    }
    $('gv-smooth').textContent = Math.round((S.smoothness || 0) * 100);
    $('gv-aim').textContent = (S.aimHeight > 0 ? '+' : '') + Math.round(S.aimHeight || 0);
    $('foot').textContent = (S.openKey ? S.openKey + ' or ' : '') + 'Esc closes the editor' +
      (S.playKey ? ' · ' + S.playKey + ' plays / pauses' : '') + (S.addKey ? ' · ' + S.addKey + ' adds a camera' : '') +
      (S.videoKey ? ' · ' + S.videoKey + ' records video' : '') + ' · Image pass: ' + (S.fxStatus || '-') +
      ' · ' + (S.videoStatus || '');
    $('lastEvent').textContent = S.lastEvent ? 'Last: ' + S.lastEvent : '';
  }

  function setOn(el, on) { if (el) el.classList.toggle('on', !!on); if (el) el.setAttribute('aria-pressed', on ? 'true' : 'false'); }

  function renderCams() {
    var n = S.cams.length;
    $('camCount').textContent = n + (n === 1 ? ' camera' : ' cameras') + ' · ' + clock(layout().total);
    var html = '';
    S.cams.forEach(function (c, i) {
      var piv = c.pivotKind >= 0 ? c.pivot + (c.bone ? ' · ' + BONES[c.bone].toLowerCase() : '') : 'no pivot';
      var mv = !hasNext(i) ? 'end' : (c.move === 3 ? 'cut to ' + (i + 2 > n ? 1 : i + 2) : MOVE_NAMES[c.move].toLowerCase() + ' ' + c.seconds.toFixed(1) + ' s');
      var cls = 'cam' + (i === sel ? ' on' : '') + (S.playing && i === S.atCam ? ' at' : '');
      html += '<button type="button" class="' + cls + '" data-sel="' + i + '" aria-pressed="' + (i === sel) + '">' +
        '<span class="badge">' + (i + 1) + '</span><span class="txt"><span class="t1">Camera ' + (i + 1) + '</span>' +
        '<span class="t2">' + esc(piv) + ' · ' + esc(mv) + '</span></span></button>';
    });
    if (!n) html = '<p class="dim">No cameras yet. Fly the free camera to a spot, open the editor and press Add camera here. Do it again somewhere else, then Play.</p>';
    $('camList').innerHTML = html;
  }

  function slider(field, label, c, note) {
    var f = F[field], v = f.get(c);
    return '<div class="slider"><label for="f-' + field + '"><span>' + label + '</span><span class="val" id="v-' + field + '">' + f.text(v) + '</span></label>' +
      '<input id="f-' + field + '" type="range" min="' + f.min + '" max="' + f.max + '" step="' + f.step + '" value="' + v + '" data-field="' + field + '">' +
      (note ? '<span class="dim small">' + note + '</span>' : '') + '</div>';
  }
  function opts(field, names, cur, cls) {
    var h = '<div class="' + cls + '" role="group">';
    names.forEach(function (nm, k) {
      h += '<button type="button" class="opt' + (k === cur ? ' on' : '') + '" aria-pressed="' + (k === cur) + '" data-opt="' + field + '" data-val="' + k + '">' + nm + '</button>';
    });
    return h + '</div>';
  }
  function check(field, label, on) {
    return '<label class="check"><input type="checkbox" data-flag="' + field + '"' + (on ? ' checked' : '') + '><span>' + label + '</span></label>';
  }

  function renderInspector() {
    var body = $('inspBody');
    var tabs = document.querySelectorAll('[data-tab]');
    for (var k = 0; k < tabs.length; k++) setOn(tabs[k], tabs[k].dataset.tab === tab);
    if (!S.cams.length) {
      $('selTitle').textContent = 'No camera';
      body.innerHTML = '<p class="dim">Add a camera first.</p>';
      return;
    }
    var c = S.cams[sel];
    $('selTitle').textContent = 'Camera ' + (sel + 1);
    var h = '';
    if (tab === 'shot') {
      h += '<div class="group"><span class="cap">Pivot - what it is about</span><div class="row">' +
        '<span class="pivot' + (c.pivotKind >= 0 ? '' : ' none') + '">' + esc(c.pivotKind >= 0 ? c.pivot : 'No pivot') + '</span>' +
        '<button type="button" class="btn" data-cam="pick">Pick</button><button type="button" class="btn" data-cam="me">Me</button>' +
        '<button type="button" class="btn" data-cam="console">Console</button><button type="button" class="btn" data-cam="none">None</button></div>';
      if (c.pivotKind >= 0) {
        h += '<label class="row" for="f-bone"><span>Aim at its</span><select id="f-bone" class="small" data-select="bone">';
        BONES.forEach(function (b, k) { h += '<option value="' + k + '"' + (k === c.bone ? ' selected' : '') + '>' + b + '</option>'; });
        h += '</select></label>';
      }
      h += '<span class="dim small">Pick: what the middle of the screen points at. Console: the person or thing clicked in the console.</span></div>';
      h += '<div class="group"><span class="cap">Looks at</span>' + opts('look', LOOKS, c.look, 'grid3') +
        (c.pivotKind < 0 && c.look === 0 ? '<span class="dim small">No pivot yet - it looks the way you set it.</span>' : '') + '</div>';
      h += slider('wait', 'Wait here', c);
      h += slider('fov', 'Zoom (field of view)', c, 'Smaller is zoomed in. Two cameras on one spot with different zoom and Crash zoom make a crash zoom.');
      h += check('blur', 'Blur the background', c.blur);
      if (c.blur) h += slider('blurStrength', 'How much', c);
    } else if (tab === 'move') {
      if (!hasNext(sel)) {
        h += '<div class="endnote">The last camera - the path ends here. Turn Loop on to go on to camera 1.</div>';
      } else {
        var nx = sel + 2 > S.cams.length ? 1 : sel + 2;
        h += '<div class="group"><span class="cap">Then to camera ' + nx + '</span>' + opts('move', MOVE_NAMES, c.move, 'grid4');
        if (c.move === 2 && c.pivotKind < 0 && (!S.cams[nx - 1] || S.cams[nx - 1].pivotKind < 0)) {
          h += '<span class="dim small">No pivot here or at camera ' + nx + ' to go round - it curves there instead.</span>';
        }
        h += '</div>';
        if (c.move !== 3) {
          var extra = c.seconds - c.time;
          h += slider('time', 'Seconds to get there', c, extra > 0.05 ? 'With its effects this move takes ' + c.seconds.toFixed(1) + ' s in all (' + extra.toFixed(1) + ' s more).' : '');
          h += curveEditor(c);
          h += '<details class="sub"><summary>More about the move</summary><div class="group">';
          if (c.move !== 2) h += slider('bendSide', 'Bend left / right', c) + slider('bendUp', 'Bend down / up', c);
          h += slider('turns', 'Extra full turns', c);
          h += '<span class="cap">Which way round</span>' + opts('way', WAYS, c.way, 'grid3');
          h += check('zoomTrick', 'Zoom trick (the pivot keeps its size while the background stretches)', c.zoomTrick);
          h += '</div></details>';
        } else {
          h += '<div class="endnote">The camera jumps straight to camera ' + nx + ', like a cut in a film.</div>';
        }
      }
    } else {
      h += slider('shake', 'Camera shake', c, 'Bumps and sways like a camera on a cart. Blends into the next camera\'s shake.');
      if (hasNext(sel) && c.move !== 3) {
        h += slider('elastic', 'Elastic arrival', c) + slider('sling', 'Slingshot', c);
        h += check('crashZoom', 'Crash zoom to the next camera', c.crashZoom);
        h += check('slowMo', 'Slow motion on the way', c.slowMo);
        if (c.slowMo) {
          h += '<div class="indent">' + slider('slowAt', 'Where (% of the way)', c) + slider('slowSpeed', 'Slow down by', c) +
            slider('slowLength', 'How much of the way', c) + check('bullet', 'Bullet time - only the world slows', !c.slowCamera);
          if (c.slowCamera) h += check('slowWorld', 'The world slows down too', c.slowWorld);
          h += '</div>';
        }
      }
    }
    body.innerHTML = h;
  }

  function curveEditor(c) {
    var cv = c.curveOn ? c.curve : (c.smooth ? [0.42, 0, 0.58, 1] : [0.33, 0.33, 0.67, 0.67]);
    var h = '<div class="group"><span class="cap">Speed curve - drag the round handles</span><div class="curvebox">' +
      '<svg id="curve" role="img" aria-label="Speed curve: time across, how far along up" width="210" height="210" viewBox="0 0 210 210">' +
      '<g stroke="#1F252E" stroke-width="1"><line x1="60.5" y1="16" x2="60.5" y2="194"></line><line x1="105" y1="16" x2="105" y2="194"></line>' +
      '<line x1="149.5" y1="16" x2="149.5" y2="194"></line><line x1="16" y1="60.5" x2="194" y2="60.5"></line><line x1="16" y1="105" x2="194" y2="105"></line>' +
      '<line x1="16" y1="149.5" x2="194" y2="149.5"></line></g><rect x="16" y="16" width="178" height="178" fill="none" stroke="#2A313B"></rect>' +
      '<line x1="16" y1="194" x2="194" y2="16" stroke="#3A424E" stroke-dasharray="4 5"></line>' +
      '<line id="cl1" x1="16" y1="194" stroke="#8C96A8" stroke-width="1.5"></line><line id="cl2" x1="194" y1="16" stroke="#8C96A8" stroke-width="1.5"></line>' +
      '<path id="cpath" stroke="' + (c.curveOn ? '#E8B04B' : '#8C96A8') + '" stroke-width="3.5" fill="none"></path>' +
      '<circle id="ch0" r="9" fill="#ECE8DF" stroke="#11151B" stroke-width="2" data-handle="0"></circle>' +
      '<circle id="ch1" r="9" fill="#ECE8DF" stroke="#11151B" stroke-width="2" data-handle="1"></circle>' +
      '<text x="105" y="208" fill="#7C8592" font-size="11" text-anchor="middle">time</text></svg><div class="presets">' +
      '<button type="button" class="chip' + (!c.curveOn ? ' on' : '') + '" data-preset="auto">Automatic</button>';
    PRESETS.forEach(function (p, k) {
      var on = c.curveOn && same(c.curve, p[1]);
      h += '<button type="button" class="chip' + (on ? ' on' : '') + '" data-preset="' + k + '">' + p[0] + '</button>';
    });
    h += '</div></div><span class="dim small">Steep is fast, flat is slow.' + (c.curveOn ? '' : ' Automatic: a slow start and stop where the camera waits or the path ends.') + '</span>';
    if (!c.curveOn) h += check('smooth', 'Automatic: always start and stop slowly', c.smooth);
    h += '</div>';
    setTimeout(function () { drawCurve(cv); }, 0);
    return h;
  }
  function same(a, b) { for (var k = 0; k < 4; k++) if (Math.abs(a[k] - b[k]) > 0.005) return false; return true; }
  function drawCurve(cv) {
    var gx = function (v) { return 16 + v * 178; }, gy = function (v) { return 194 - v * 178; };
    var p = $('cpath'); if (!p) return;
    p.setAttribute('d', 'M 16 194 C ' + gx(cv[0]) + ' ' + gy(cv[1]) + ', ' + gx(cv[2]) + ' ' + gy(cv[3]) + ', 194 16');
    $('cl1').setAttribute('x2', gx(cv[0])); $('cl1').setAttribute('y2', gy(cv[1]));
    $('cl2').setAttribute('x2', gx(cv[2])); $('cl2').setAttribute('y2', gy(cv[3]));
    $('ch0').setAttribute('cx', gx(cv[0])); $('ch0').setAttribute('cy', gy(cv[1]));
    $('ch1').setAttribute('cx', gx(cv[2])); $('ch1').setAttribute('cy', gy(cv[3]));
  }

  function renderTimeline() {
    var lay = layout(), W = $('lane').offsetWidth || 1832;
    var x = function (t) { return t / lay.total * W; };
    var h = '';
    lay.segs.forEach(function (s) {
      if (s.kind === 'cut') {
        h += '<div class="seg cut" style="left:' + (x(s.t) + 2) + 'px"><span class="lbl">Cut</span></div>';
        return;
      }
      var w = Math.max(x(s.d) - 2, 3);
      var band = '';
      if (s.slow) {
        band = '<span class="slowband" style="left:' + Math.max(0, (s.slow.slowAt - s.slow.slowLength / 2) * w) + 'px;width:' + s.slow.slowLength * w + 'px"></span>';
      }
      h += '<div class="seg ' + s.kind + '" style="left:' + x(s.t) + 'px;width:' + w + 'px">' + band +
        '<span class="lbl">' + (w > 70 ? esc(s.label) : '') + '</span></div>';
    });
    $('lane').innerHTML = h;
    var m = '';
    lay.marks.forEach(function (mk) {
      m += '<button type="button" class="mark' + (mk.i === sel ? ' on' : '') + '" style="left:' + x(mk.t) + 'px" data-sel="' + mk.i + '" aria-label="Camera ' + (mk.i + 1) + '">' + (mk.i + 1) + '</button>';
    });
    $('marks').innerHTML = m;
    var step = lay.total > 60 ? 10 : lay.total > 30 ? 5 : lay.total > 12 ? 2 : 1, r = '';
    for (var s = 0; s <= lay.total + 0.001; s += step) r += '<div class="tick" style="left:' + x(s) + 'px"><i></i><span>' + s + 's</span></div>';
    $('ruler').innerHTML = r;
    $('scrub').max = lay.total.toFixed(2);
  }

  function renderTransport() {
    var lay = layout(), W = $('lane').offsetWidth || 1832;
    var cur = scrubbing ? scrubAt : (S.playing ? S.progress * lay.total : scrubAt);
    cur = Math.min(Math.max(cur, 0), lay.total);
    $('playhead').style.left = (cur / lay.total * W) + 'px';
    if (!scrubbing) $('scrub').value = cur;
    $('time').textContent = clock(cur) + ' / ' + clock(lay.total);
    var at = camAt(lay, cur);
    var n = S.cams.length;
    $('nowLabel').textContent = !n ? '' : (S.playing ? (S.paused ? 'Paused - ' : S.rewinding ? (S.rate < -1.5 ? 'Rewinding - ' : 'Backwards - ') : 'Playing - ') : '') +
      (at.moving ? 'camera ' + (at.cam + 1) + ' on to camera ' + (at.cam + 2 > n ? 1 : at.cam + 2) : 'at camera ' + (at.cam + 1));
    var playing = S.playing && !S.paused && !S.rewinding;
    $('icPlay').style.display = playing ? 'none' : '';
    $('icPause').style.display = playing ? '' : 'none';
    $('btPlay').setAttribute('aria-label', playing ? 'Pause' : 'Play');
  }

  // ---- input --------------------------------------------------------------------------------------------------------
  function local(field, value) { if (S && S.cams[sel]) S.cams[sel][field] = value; }

  document.addEventListener('click', function (e) {
    var el = e.target.closest('button, [data-sel]');
    if (!el || !S) return;
    if (el.dataset.sel !== undefined) { sel = +el.dataset.sel; render(true); return; }
    if (el.dataset.tab) { tab = el.dataset.tab; render(true); return; }
    if (el.dataset.toggle) { var k = el.dataset.toggle; S[k] = !S[k]; cmd(k + '|' + (S[k] ? 1 : 0)); renderTop(); return; }
    if (el.dataset.cam) { cmd(el.dataset.cam + '|' + sel); return; }
    if (el.dataset.opt) {
      var f = el.dataset.opt, v = +el.dataset.val;
      local(f, v); cmd('edit|' + sel + '|' + f + '|' + v); render(true); return;
    }
    if (el.dataset.preset !== undefined) {
      var c = S.cams[sel];
      if (el.dataset.preset === 'auto') { c.curveOn = false; cmd('edit|' + sel + '|curveOn|0'); }
      else { var p = PRESETS[+el.dataset.preset][1]; c.curveOn = true; c.curve = p.slice(); cmd('edit|' + sel + '|curve|' + p.join('|')); }
      render(true); return;
    }
    if (el.id === 'btClear') {
      if (!clearArmed) { clearArmed = true; el.textContent = 'Click again to clear all ' + S.cams.length + ' cameras'; setTimeout(function () { clearArmed = false; el.textContent = 'Clear all cameras'; }, 3000); return; }
      clearArmed = false; el.textContent = 'Clear all cameras'; cmd('clear'); return;
    }
    if (el.dataset.cmd) {
      var c2 = el.dataset.cmd;
      if (c2 === 'saveAs') { var name = $('saveName').value.trim(); if (name) cmd('saveAs|' + name.replace(/\|/g, ' ')); return; }
      if (c2 === 'record' && S.recordingPath) { cmd('record'); return; }
      cmd(c2);
    }
  });

  document.addEventListener('input', function (e) {
    var el = e.target;
    if (el.id === 'scrub') {
      scrubbing = true; scrubAt = +el.value; renderTransport();
      throttled('seek', function () { cmd('seek|' + scrubAt.toFixed(2)); });
      return;
    }
    if (el.dataset && el.dataset.global && S) {  // path-wide: smoothness, aim height
      busy = true;
      var g = el.dataset.global, gv = g === 'smoothness' ? +el.value / 100 : +el.value;
      S[g] = gv;
      $(g === 'smoothness' ? 'gv-smooth' : 'gv-aim').textContent = g === 'smoothness' ? el.value : (gv > 0 ? '+' : '') + el.value;
      throttled('g-' + g, function () { cmd(g + '|' + gv); });
      return;
    }
    var field = el.dataset && el.dataset.field;
    if (!field || !S || !S.cams[sel]) return;
    busy = true;
    var f = F[field], v = +el.value;
    $('v-' + field).textContent = f.text(v);
    local(field, f.put(v));
    throttled('edit-' + field, function () { cmd('edit|' + sel + '|' + field + '|' + f.put(v)); });
  });

  document.addEventListener('change', function (e) {
    var el = e.target;
    if (el.id === 'scrub') { scrubbing = false; scrubAt = +el.value; cmd('seek|' + scrubAt.toFixed(2)); return; }
    if (el.id === 'open') { if (el.value) cmd('open|' + el.value); el.value = ''; return; }
    if (el.id === 'watch') { cmd('watch|' + (el.checked ? 1 : 0)); return; }
    if (el.dataset.global) { busy = false; cmd(el.dataset.global + '|' + (el.dataset.global === 'smoothness' ? +el.value / 100 : +el.value)); return; }
    if (!S || !S.cams[sel]) return;
    if (el.dataset.field) {
      var f = F[el.dataset.field];
      cmd('edit|' + sel + '|' + el.dataset.field + '|' + f.put(+el.value));
      busy = false; render(true); return;
    }
    if (el.dataset.flag) {
      var flag = el.dataset.flag, on = el.checked;
      if (flag === 'bullet') { local('slowCamera', !on); cmd('edit|' + sel + '|slowCamera|' + (on ? 0 : 1)); }
      else { local(flag, on); cmd('edit|' + sel + '|' + flag + '|' + (on ? 1 : 0)); }
      render(true); return;
    }
    if (el.dataset.select === 'bone') { local('bone', +el.value); cmd('edit|' + sel + '|bone|' + el.value); render(true); }
  });

  document.addEventListener('pointerup', function () {
    if (busy && dragHandle < 0) { busy = false; }
    if (dragHandle >= 0) {
      var c = S && S.cams[sel];
      if (c) cmd('edit|' + sel + '|curve|' + c.curve.map(function (v) { return v.toFixed(3); }).join('|'));
      dragHandle = -1; busy = false; render(true);
    }
  });
  document.addEventListener('pointerdown', function (e) {
    var h = e.target.closest && e.target.closest('[data-handle]');
    if (h && S && S.cams[sel]) {
      var c = S.cams[sel];
      if (!c.curveOn) { c.curve = c.smooth ? [0.42, 0, 0.58, 1] : [0.33, 0.33, 0.67, 0.67]; c.curveOn = true; }
      dragHandle = +h.dataset.handle; busy = true; e.preventDefault();
    }
  });
  document.addEventListener('pointermove', function (e) {
    if (dragHandle < 0 || !S) return;
    var svg = $('curve'); if (!svg) return;
    var r = svg.getBoundingClientRect();
    var px = (e.clientX - r.left) / r.width * 210, py = (e.clientY - r.top) / r.height * 210;
    var vx = Math.min(1, Math.max(0, (px - 16) / 178)), vy = Math.min(1, Math.max(0, (194 - py) / 178));
    var c = S.cams[sel];
    c.curve[dragHandle * 2] = Math.round(vx * 100) / 100;
    c.curve[dragHandle * 2 + 1] = Math.round(vy * 100) / 100;
    drawCurve(c.curve);
    $('cpath').setAttribute('stroke', '#E8B04B');
    throttled('curve', function () { cmd('edit|' + sel + '|curve|' + c.curve.join('|')); });
  });

  // text boxes: the game's keys stay out while typing
  document.addEventListener('focusin', function (e) { if (e.target.tagName === 'INPUT' && e.target.type === 'text') cmd('typing|1'); });
  document.addEventListener('focusout', function (e) { if (e.target.tagName === 'INPUT' && e.target.type === 'text') cmd('typing|0'); });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && document.activeElement && document.activeElement.type === 'text') document.activeElement.blur();
    if (e.key === 'Enter' && document.activeElement && document.activeElement.id === 'saveName') $('saveName').nextElementSibling.click();
  });

  // ---- from TFCam ------------------------------------------------------------------------------------------------------
  window.tfcamState = function (json) {
    var st;
    try { st = typeof json === 'string' ? JSON.parse(json) : json; } catch (err) { console.error('TFCam editor: bad state ' + err); return; }
    S = st;
    if (!scrubbing && !S.playing) scrubAt = Math.min(scrubAt, S.total || 0);
    render(false);
  };

  function fit() {
    var s = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
    $('app').style.transform = 'scale(' + s + ')';
  }
  window.addEventListener('resize', fit);
  fit();

  // ---- outside the game: demo data ---------------------------------------------------------------------------------------
  function cam(o) {
    var base = { pivot: 'none', pivotKind: -1, bone: 0, look: 1, wait: 0, fov: 70, move: 1, time: 3, seconds: 3, smooth: false, turns: 0, way: 0, zoomTrick: false,
      bendSide: 0, bendUp: 0, blur: false, blurStrength: 0.6, shake: 0, crashZoom: false, slowMo: false, slowAt: 0.66, slowSpeed: 0.4, slowLength: 0.2,
      slowWorld: false, slowCamera: true, elastic: 0, sling: 0, curveOn: false, curve: [0.42, 0, 0.58, 1] };
    for (var k in o) base[k] = o[k];
    return base;
  }
  function previewCmd(s) {
    var a = s.split('|');
    if (a[0] === 'play') { S.playing = !(S.playing && !S.paused); S.paused = false; }
    else if (a[0] === 'stop') { S.playing = false; }
    else if (a[0] === 'rewind') { S.playing = false; S.progress = 0; scrubAt = 0; }
    else if (a[0] === 'freeze') { S.frozen = !S.frozen; }
    else if (a[0] === 'video') { S.video = !S.video; }
    else if (a[0] === 'seek') { S.playing = false; scrubAt = +a[1]; }
    S.lastEvent = s;
    render(false);
  }
  setTimeout(function () {
    if (/Ultralight/.test(navigator.userAgent) || typeof window.tfcamCmd === 'function' || S) { cmd('ready'); return; }
    preview = true;
    if (location.hash.length > 1) tab = location.hash.slice(1);
    var badge = document.createElement('div');
    badge.className = 'preview';
    badge.textContent = 'Preview outside the game - demo data';
    document.body.appendChild(badge);
    S = {
      name: 'Whiterun - Adrianne', follow: true, loop: false, showPath: true, smoothness: 0.3, aimHeight: 0, playing: false, paused: false, rewinding: false,
      rate: 1, atCam: -1, progress: 0, total: 9.7, freeCam: true, frozen: false, clean: false, recordingPath: false, video: false,
      videoStatus: 'not recording', fxStatus: 'idle', fixed: false, watching: false, lastEvent: 'camera 4 added', openKey: '1', playKey: '', addKey: '', videoKey: '',
      saved: ['whiterun', 'Before clearing'],
      cams: [
        cam({ pivot: 'Adrianne', pivotKind: 1, bone: 1, look: 0, wait: 0.2, fov: 62, move: 2, time: 1.9, seconds: 2.2, slowMo: true, curveOn: true }),
        cam({ pivot: 'Adrianne', pivotKind: 1, look: 0, fov: 55, move: 1, time: 3, seconds: 3, bendSide: 0.25, bendUp: 0.1, shake: 0.15, elastic: 0.3, curveOn: true, curve: [0.1, 0.7, 0.3, 1] }),
        cam({ look: 1, wait: 1, move: 3, seconds: 0 }),
        cam({ pivot: 'you', pivotKind: 0, bone: 2, look: 0, wait: 1.5, fov: 40, crashZoom: true })
      ]
    };
    S.total = layout().total;
    scrubAt = 2.9;
    render(true);
  }, 800);
})();
