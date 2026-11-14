// TFCam Camera Path editor (PrismaUI view). TFCam pushes its state with tfcamState(json) about ten times a second while
// the editor is open; every change goes back through tfcamCmd("cmd|arg|..."), handled on the game's main thread.
// Minimal layout: a bar at the bottom (play, the timeline, add, record, menu); picking a camera on the timeline opens a
// small card above it; everything for the whole path is in the menu. Outside the game it shows demo data.
// PrismaUI notes: mouse events only (mousemove carries no button state, so drags run from our mousedown to our mouseup),
// no CSS transform (clicks land off target), no stroked SVG (drawn broken) - the speed curve is drawn on a canvas.
(function () {
  'use strict';

  var MOVES = ['straight', 'curve', 'around', 'cut'];
  var MOVE_NAMES = ['Straight', 'Curve', 'Orbit', 'Cut'];
  var LOOKS = ['The pivot', 'Fixed', 'Direction of travel'];
  var BONES = ['Middle', 'Head', 'Eyes', 'Chest', 'Hips', 'Bum', 'Groin', 'Hands', 'Feet'];

  // camera options shown as sliders: the slider's own number <-> the camera's value
  var F = {
    wait: { min: 0, max: 10, step: 0.1, get: function (c) { return c.wait; }, put: function (v) { return v; }, text: function (v) { return v.toFixed(1) + ' s'; } },
    fov: { min: 5, max: 150, step: 1, get: function (c) { return c.fov; }, put: function (v) { return v; }, text: function (v) { return Math.round(v) + '°'; } },
    time: { min: 0.5, max: 30, step: 0.1, get: function (c) { return c.time; }, put: function (v) { return v; }, text: function (v) { return v.toFixed(1) + ' s'; } },
    bendSide: pct(-100, 100, 'bendSide', true), bendUp: pct(-100, 100, 'bendUp', true),
    turns: { min: 0, max: 10, step: 1, get: function (c) { return c.turns; }, put: function (v) { return Math.round(v); }, text: function (v) { return String(Math.round(v)); } },
    shake: pct(0, 100, 'shake'), elastic: pct(0, 100, 'elastic'), sling: pct(0, 100, 'sling'),
    blurStrength: pct(10, 100, 'blurStrength'), dolly: pct(-100, 100, 'dolly', true),
    // the slow down: how long it lasts on screen, how slow (see slowEdit)
    slowFor: { min: 0.2, max: function (c) { return Math.max(0.3, Math.floor(c.time * (1 - slowStart(c)) / Math.sqrt(c.slowSpeed) * 10) / 10); }, step: 0.1,
      get: function (c) { return Math.round(c.time * c.slowLength / Math.sqrt(c.slowSpeed) * 10) / 10; }, put: function (v) { return v; }, text: function (v) { return v.toFixed(1) + ' s'; } },
    slowHow: { min: 5, max: 90, step: 1, get: function (c) { return Math.round(c.slowSpeed * 100); }, put: function (v) { return v / 100; }, text: function (v) { return Math.round(v) + '% speed'; } }
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
  var sel = -1;           // the picked camera; -1 = none, the card is closed
  var expanded = false;   // the card shows every setting (tabs) instead of the few common ones
  var tab = 'move';
  var menuOpen = false;
  var busy = false;       // a slider or the curve is being dragged: the card is not redrawn under it
  var scrubbing = false;
  var scrubAt = 0;
  var dragHandle = -1;
  var lastKey = '';
  var clearArmed = false;
  var sceneDelArmed = false;   // SexLab scene path delete: armed by the first click
  var preview = false;
  var seenEvent = null, flash = '', flashUntil = 0, flashWarn = false;

  function $(id) { return document.getElementById(id); }
  function show(el, on) { if (el) el.style.display = on ? '' : 'none'; }
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
  function nextOf(i) { return i + 2 > S.cams.length ? 1 : i + 2; }
  function layout() {
    var out = { segs: [], marks: [], total: 0 }, t = 0;
    S.cams.forEach(function (c, i) {
      out.marks.push({ i: i, t: t });
      if (c.wait > 0) out.segs.push({ kind: 'wait', t: t, d: c.wait, cam: i });
      t += c.wait;
      if (hasNext(i)) {
        if (c.move === 3) {
          out.segs.push({ kind: 'cut', t: t, d: 0, cam: i });
        } else {
          out.segs.push({ kind: MOVES[c.move], t: t, d: c.seconds, cam: i, slow: c.slowMo ? slowSpan(c) : null });
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
  function moveText(c, i) {
    if (!hasNext(i)) return 'the path ends here';
    if (c.move === 3) return 'cut to ' + nextOf(i);
    var t = MOVE_NAMES[c.move].toLowerCase() + ' to ' + nextOf(i) + ', ' + c.seconds.toFixed(1) + ' s';
    if (c.slowMo) t += c.slowCamera ? ', slows down' : ', bullet time';
    return t;
  }

  // The slow down is a dip in speed from slowAt - slowLength / 2 over slowLength of the way (normal time); on screen it
  // lasts time x length / sqrt(speed) (the dip's average). Where it sits in the move's own seconds, for the timeline:
  function slowStart(c) { return Math.max(0, c.slowAt - c.slowLength / 2); }
  function slowSpan(c) {
    var wind = c.sling > 0.001 ? 0.4 + 0.5 * c.sling : 0;  // the slingshot's wind-up comes first (TFCam's WindTime)
    return { at: wind + slowStart(c) * c.time, d: c.time * c.slowLength / (c.slowCamera ? Math.sqrt(c.slowSpeed) : 1) };
  }
  // the move the paused path is in, for Slow down here; -1 = none
  function slowCam(lay, t) {
    if (!S.playing || !S.paused || scrubbing) return -1;
    for (var k = 0; k < lay.segs.length; k++) {
      var s = lay.segs[k];
      if (s.kind !== 'wait' && s.kind !== 'cut' && t > s.t + 0.02 && t < s.t + s.d - 0.05) return s.cam;
    }
    return -1;
  }

  // ---- drawing --------------------------------------------------------------------------------------------------------
  function render(force) {
    if (!S) return;
    var n = S.cams.length;
    if (sel >= n) sel = n - 1;
    var key = JSON.stringify([S.cams, S.loop, sel, tab, expanded, S.atCam, S.playing]);
    renderBar();
    renderMenu();
    if (force || key !== lastKey) {
      renderTrack();  // before the card: the card sits over its camera's mark
      // a change that came while a slider was held is drawn as soon as it is let go - the key is only kept once the
      // card is drawn too (otherwise cleared / added cameras would only show after reopening the editor)
      if (!busy) { renderCard(); lastKey = key; }
    }
    renderTransport();
    renderToast();
  }

  function setOn(el, on) { if (!el) return; el.classList.toggle('on', !!on); el.setAttribute('aria-pressed', on ? 'true' : 'false'); }

  function renderBar() {
    setOn($('btVideo'), S.video);
    $('btVideo').title = S.video ? 'Stop the video' + (S.videoStatus ? ' (' + S.videoStatus + ')' : '') : 'Record video';
    $('btVideo').setAttribute('aria-label', S.video ? 'Stop the video' : 'Record video');
    $('btAdd').disabled = !S.freeCam || S.recordingPath;
    setOn($('btMenu'), menuOpen);
    setOn($('tgFreeze'), S.frozen);  // on the bar: set things up, freeze, play
  }

  function renderMenu() {
    show($('menu'), menuOpen);
    if (!menuOpen) return;
    setOn($('tgFollow'), S.follow);
    setOn($('tgLoop'), S.loop);
    setOn($('tgPath'), S.showPath);
    setOn($('tgClean'), S.clean);
    var open = $('open');
    var names = (S.saved || []).join('\n') + '|' + S.name;
    if (open.dataset.names !== names) {
      open.dataset.names = names;
      var html = '<option value="">' + esc(S.name || 'This path') + '</option>';
      (S.saved || []).forEach(function (p) { html += '<option value="' + esc(p) + '">Open: ' + esc(p) + '</option>'; });
      open.innerHTML = html;
      open.value = '';
    }
    $('btRecordPath').textContent = S.recordingPath ? 'Stop recording the path' : 'Record a manual path (fly it)';
    var fix = $('btFix');
    fix.textContent = S.fixed ? 'Let the camera go' : 'Fix the camera here';
    fix.dataset.cmd = S.fixed ? 'release' : 'fix';
    $('watch').checked = !!S.watching;
    // SexLab scene paths - only while a stage of your scene is known
    show($('sceneGroup'), !!S.sceneIn);
    if (S.sceneIn) {
      $('sceneLabel').textContent = S.sceneLabel + (S.scenePlaying ? ' · its path plays (loops until the stage changes)' :
        (S.sceneSaved ? ' · has a saved path' : ' · no saved path yet'));
      show($('btSceneOpen'), !!S.sceneSaved);
      show($('btScenePlay'), !!S.sceneSaved && !S.scenePlaying);
      show($('btSceneDel'), !!S.sceneSaved);
      $('btSceneSave').textContent = S.sceneSaved ? 'Save this path over this stage’s path' : 'Save this path for this stage';
      setOn($('tgSceneAuto'), S.sceneAuto !== false);
    }
    if (!busy) {
      $('g-smooth').value = Math.round((S.smoothness || 0) * 100);
      $('g-aim').value = Math.round(S.aimHeight || 0);
    }
    $('gv-smooth').textContent = Math.round((S.smoothness || 0) * 100);
    $('gv-aim').textContent = (S.aimHeight > 0 ? '+' : '') + Math.round(S.aimHeight || 0);
    $('foot').textContent = (S.openKey ? S.openKey + ' or ' : '') + 'Esc closes the editor' +
      (S.playKey ? ' · ' + S.playKey + ' plays / pauses' : '') + (S.addKey ? ' · ' + S.addKey + ' adds a camera' : '') +
      (S.videoKey ? ' · ' + S.videoKey + ' records video' : '') + '. Image effects: ' + (S.fxStatus || '-') +
      '. Video: ' + (S.videoStatus || '-') + '.';
  }

  function renderToast() {
    var ev = S.lastEvent || '';
    if (seenEvent === null) {
      seenEvent = ev;  // old news when the editor opens: not shown
    } else if (ev !== seenEvent) {
      seenEvent = ev;
      // "go to <place> first" (a camera made somewhere else) stays longer, in the warning colour
      flashWarn = / first\b/.test(ev);
      var ms = flashWarn ? 6000 : 2500;
      if (ev) { flash = ev.charAt(0).toUpperCase() + ev.slice(1); flashUntil = Date.now() + ms; setTimeout(function () { if (S) renderToast(); }, ms + 100); }
    }
    var notice = S.recordingPath ? 'Recording a manual path - close the editor and fly; open it again to stop'
      : (!S.freeCam ? 'Turn the free camera on to add cameras and play the path' : '');
    var flashing = Date.now() < flashUntil;
    var msg = notice || (flashing ? flash : '');
    show($('toast'), !!msg);
    $('toast').classList.toggle('warn', !!notice || (flashing && flashWarn));
    $('toastText').textContent = msg;
  }

  function trackWidth() { return $('track').offsetWidth || 1200; }

  function renderTrack() {
    var lay = layout(), W = trackWidth();
    var x = function (t) { return t / lay.total * W; };
    var h = '';
    lay.segs.forEach(function (s) {
      if (s.kind === 'cut') { h += '<i class="seg cut" style="left:' + x(s.t) + 'px"></i>'; return; }
      var w = Math.max(x(s.d) - 2, 2), d = Math.max(s.d, 0.001);
      var band = s.slow ? '<b class="slowband" style="left:' + Math.min(s.slow.at / d, 1) * w +
        'px;width:' + Math.min(s.slow.d / d, 1) * w + 'px"></b>' : '';
      h += '<i class="seg ' + s.kind + (s.cam === sel && s.kind !== 'wait' ? ' sel' : '') + '" style="left:' + (x(s.t) + 1) +
        'px;width:' + w + 'px">' + band + '</i>';
    });
    $('lane').innerHTML = h;
    // a number where there is room for it, else a dot; the picked camera always gets its number
    var xs = lay.marks.map(function (mk) { return x(mk.t); }), m = '';
    lay.marks.forEach(function (mk, k) {
      var room = Math.min(k > 0 ? xs[k] - xs[k - 1] : 99, k < xs.length - 1 ? xs[k + 1] - xs[k] : 99);
      var big = mk.i === sel || room >= 24;
      var cls = 'mark' + (big ? '' : ' dot') + (mk.i === sel ? ' on' : '') + (S.playing && mk.i === S.atCam ? ' at' : '');
      m += '<button type="button" class="' + cls + '" style="left:' + xs[k] + 'px" data-sel="' + mk.i + '" title="Camera ' + (mk.i + 1) +
        '" aria-label="Camera ' + (mk.i + 1) + '">' + (big ? mk.i + 1 : '') + '</button>';
    });
    $('marks').innerHTML = m;
  }

  function renderTransport() {
    var lay = layout();
    var cur = scrubbing ? scrubAt : (S.playing ? S.progress * lay.total : scrubAt);
    cur = Math.min(Math.max(cur, 0), lay.total);
    $('playhead').style.left = (cur / lay.total * trackWidth()) + 'px';
    $('time').textContent = clock(cur) + ' / ' + clock(lay.total);
    var playing = S.playing && !S.paused && !S.rewinding;
    show($('icPlay'), !playing);
    show($('icPause'), playing);
    $('btPlay').setAttribute('aria-label', playing ? 'Pause' : 'Play');
    var back = !!(S.playing && S.rewinding);  // going backwards: the backwards button pauses
    show($('icBack'), !back);
    show($('icBackPause'), back);
    setOn($('btBack'), back);
    $('btBack').title = back ? 'Pause' : 'Play backwards';
    $('btBack').setAttribute('aria-label', back ? 'Pause' : 'Play backwards');
    var sc = slowCam(lay, cur), bs = $('btSlow');
    bs.disabled = sc < 0;
    bs.dataset.at = sc;
    bs.title = sc < 0 ? 'Pause during a move, then press this to slow the camera down from there'
      : 'Slow the camera down from here, on the way from ' + (sc + 1) + ' to ' + nextOf(sc);
  }

  function slider(field, label, c, note) {
    var f = F[field], v = f.get(c), max = typeof f.max === 'function' ? f.max(c) : f.max;
    return '<div class="slider"><label for="f-' + field + '"><span>' + label + '</span><span class="val" id="v-' + field + '">' + f.text(v) + '</span></label>' +
      '<input id="f-' + field + '" type="range" min="' + f.min + '" max="' + max + '" step="' + f.step + '" value="' + v + '" data-field="' + field + '">' +
      (note ? '<span class="dim small">' + note + '</span>' : '') + '</div>';
  }
  function opts(field, names, cur) {
    var h = '<div class="segbtns" role="group">';
    names.forEach(function (nm, k) {
      h += '<button type="button" class="opt' + (k === cur ? ' on' : '') + '" aria-pressed="' + (k === cur) + '" data-opt="' + field + '" data-val="' + k + '">' + nm + '</button>';
    });
    return h + '</div>';
  }
  // an orbit goes left or right; a camera still on "the short way" (0, older paths) shows the way it goes (wayNow)
  function wayOpts(c) {
    var cur = c.way || c.wayNow || 0, h = '<div class="segbtns" role="group">';
    [[1, 'Left'], [2, 'Right']].forEach(function (w) {
      h += '<button type="button" class="opt' + (w[0] === cur ? ' on' : '') + '" aria-pressed="' + (w[0] === cur) + '" data-opt="way" data-val="' + w[0] + '">' + w[1] + '</button>';
    });
    return h + '</div>';
  }
  function check(field, label, on) {
    return '<label class="check"><input type="checkbox" data-flag="' + field + '"' + (on ? ' checked' : '') + '><span>' + label + '</span></label>';
  }

  function renderCard() {
    var card = $('card');
    if (sel < 0 || !S.cams.length) { show(card, false); return; }
    show(card, true);
    card.classList.toggle('wide', expanded);
    var c = S.cams[sel];
    $('cNum').textContent = sel + 1;
    $('cTitle').textContent = 'Camera ' + (sel + 1) + (c.pivotKind >= 0 ? ' · ' + c.pivot : '');
    $('cSub').textContent = moveText(c, sel) + (c.wait > 0 ? ' · waits ' + c.wait.toFixed(1) + ' s' : '');
    $('cBody').innerHTML = expanded ? full(c) : quick(c);
    $('btMore').textContent = expanded ? 'Fewer settings' : 'More settings';
    if (expanded && tab !== 'shot') drawCurve(curveOf(c), c.curveOn);
    placeCard();
  }

  // the few settings most moves need
  function quick(c) {
    var h = '';
    if (hasNext(sel)) {
      h += '<div class="group"><span class="cap">Then to camera ' + nextOf(sel) + '</span>' + opts('move', MOVE_NAMES, c.move) + '</div>';
      if (c.move !== 3) h += slider('time', 'Seconds to get there', c) + (c.slowMo ? slowGroup(c) : '');
    } else {
      h += '<div class="note">The last camera: the path ends here.' + (S.loop ? '' : ' Turn Loop on in the menu to go on to camera 1.') + '</div>';
    }
    h += slider('wait', 'Wait here', c);
    return h;
  }

  // what happens while the camera waits here and stares
  function waitFx(c) {
    var h = check('zoomTrick', 'Dolly (the pivot keeps its size, the background stretches)', c.zoomTrick);
    if (c.zoomTrick) {
      h += '<div class="indent">' + slider('dolly', 'Pull back / push in while waiting', c,
        c.wait <= 0 ? 'Needs a wait here.' : (c.pivotKind < 0 ? 'No pivot: what is at your distance keeps its size.' : '')) + '</div>';
    }
    h += slider('shake', 'Camera shake', c);
    h += check('blur', 'Blur the background', c.blur);
    if (c.blur) h += '<div class="indent">' + slider('blurStrength', 'How much', c,
      c.pivotKind < 0 ? 'No pivot: what is at your distance stays sharp.' : '') + '</div>';
    return h;
  }

  // the slow down on the way on: made by Slow down here (pause during the move), then how long and how slow
  function slowGroup(c) {
    return '<div class="group"><span class="cap">Slow down</span>' + slider('slowFor', 'How long', c) + slider('slowHow', 'How slow', c) +
      '<button type="button" class="btn" data-slow="off">Remove the slow down</button></div>';
  }

  // every setting, in two tabs
  function full(c) {
    var h = '<div class="tabs" role="group">';
    [['shot', 'Shot'], ['move', 'Move']].forEach(function (t) {
      h += '<button type="button" class="' + (tab === t[0] ? 'on' : '') + '" data-tab="' + t[0] + '">' + t[1] + '</button>';
    });
    h += '</div>';
    if (tab === 'shot') {
      h += '<div class="group"><div class="row">' +
        '<span class="pivot' + (c.pivotKind >= 0 ? '' : ' none') + '">' + esc(c.pivotKind >= 0 ? c.pivot : 'No pivot') + '</span>' +
        '<button type="button" class="btn" data-cam="pick">Pick</button><button type="button" class="btn" data-cam="me">Me</button>' +
        '<button type="button" class="btn" data-cam="console">Console</button><button type="button" class="btn" data-cam="none">None</button></div>';
      if (c.pivotKind >= 0) {
        h += '<label class="row" for="f-bone"><span>Aim at its</span><select id="f-bone" data-select="bone">';
        BONES.forEach(function (b, k) { h += '<option value="' + k + '"' + (k === c.bone ? ' selected' : '') + '>' + b + '</option>'; });
        h += '</select></label>';
      }
      h += '<span class="dim small">Pick: what the middle of the screen points at. Console: opens the console - click who you want, then close it.</span></div>';
      h += '<div class="group"><span class="cap">Looks at</span>' + opts('look', LOOKS, c.look) +
        (c.pivotKind < 0 && c.look === 0 ? '<span class="dim small">No pivot yet - it stays fixed.</span>' : '') + '</div>';
      h += slider('fov', 'Zoom (field of view)', c, 'Smaller is zoomed in.');
      h += slider('wait', 'Wait here', c);
      h += waitFx(c);
    } else {
      if (!hasNext(sel)) {
        h += '<div class="note">The last camera: the path ends here. Turn Loop on to go on to camera 1.</div>';
      } else {
        var nx = nextOf(sel);
        h += '<div class="group"><span class="cap">Then to camera ' + nx + '</span>' + opts('move', MOVE_NAMES, c.move);
        if (c.move === 2 && c.pivotKind < 0 && (!S.cams[nx - 1] || S.cams[nx - 1].pivotKind < 0)) {
          h += '<span class="dim small">No pivot here or at camera ' + nx + ' to orbit - it curves there instead.</span>';
        }
        h += '</div>';
        if (c.move === 2) h += '<div class="group"><span class="cap">Which way round</span>' + wayOpts(c) + '</div>' + slider('turns', 'Extra full turns', c);
        if (c.move !== 3) {
          var extra = c.seconds - c.time;
          h += slider('time', 'Seconds to get there', c, extra > 0.05 ? 'With its effects it takes ' + c.seconds.toFixed(1) + ' s.' : '');
          h += curveEditor(c);
          if (c.move !== 2) h += slider('bendSide', 'Bend left / right', c) + slider('bendUp', 'Bend down / up', c);
          h += c.slowMo ? slowGroup(c) : '<span class="dim small">To slow part of the way down: play, pause where it should start, then press Slow down here.</span>';
          h += slider('elastic', 'Elastic arrival', c) + slider('sling', 'Slingshot', c);
          h += check('crashZoom', 'Crash zoom to the next camera', c.crashZoom);
        } else {
          h += '<div class="note">The camera jumps straight to camera ' + nx + ', like a cut in a film.</div>';
        }
      }
    }
    return h;
  }

  // The automatic speed as a curve (TFCam's Ease, a cubic Hermite = this bezier): a slow start where the camera waits
  // (or the path starts), a slow stop where the next one waits (or the path ends), "smooth" both.
  function curveOf(c) {
    if (c.curveOn) return c.curve;
    var n = S.cams.length, nx = nextOf(sel) - 1, cn = S.cams[nx] || c;
    var slowIn = c.sling <= 0.001 && (c.smooth || c.wait > 0 || (!S.loop && sel === 0));
    var slowOut = c.elastic <= 0.001 && (c.smooth || cn.wait > 0 || (!S.loop && nx === n - 1));
    return [1 / 3, slowIn ? 0 : 1 / 3, 2 / 3, slowOut ? 1 : 2 / 3];
  }
  function curveEditor(c) {
    return '<div class="group"><span class="cap">Speed - drag the round handles</span><div class="curvebox">' +
      '<canvas id="curve" width="160" height="160" aria-label="Speed curve: time across, how far along up"></canvas><div class="curveside">' +
      '<span class="dim small">Time goes across, the way up. Steep is fast, flat is slow.</span>' +
      (c.curveOn ? '<button type="button" class="btn" data-preset="auto" title="Back to the automatic speed">Reset</button>'
        : '<span class="dim small">Automatic until you drag a handle.</span>') +
      '</div></div></div>';
  }
  function curveGeom(cnv) {
    var w = cnv.width, p = Math.round(w * 0.09), l = w - 2 * p;
    return { w: w, gx: function (v) { return p + v * l; }, gy: function (v) { return w - p - v * l; }, p: p, l: l };
  }
  function drawCurve(cv, hot) {
    var cnv = $('curve');
    if (!cnv || !cnv.getContext) return;
    var size = Math.round(cnv.offsetWidth || 160);
    if (cnv.width !== size) { cnv.width = size; cnv.height = size; }
    var ctx = cnv.getContext('2d'), g = curveGeom(cnv), k, t;
    ctx.clearRect(0, 0, g.w, g.w);
    ctx.fillStyle = '#1C222B';  // grid as filled 1 px rectangles
    for (k = 0; k <= 4; k++) {
      var at = Math.round(g.p + k * g.l / 4);
      ctx.fillRect(at, g.p, 1, g.l);
      ctx.fillRect(g.p, at, g.l, 1);
    }
    var dot = function (x, y, r, col) { ctx.fillStyle = col; ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fill(); };
    for (t = 0; t <= 1.0001; t += 0.05) dot(g.gx(t), g.gy(t), 1, '#3A424E');
    var line = function (x0, y0, x1, y1, col) {
      var n = Math.max(2, Math.round(Math.hypot(x1 - x0, y1 - y0) / 4));
      for (var i = 0; i <= n; i++) dot(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, 0.9, col);
    };
    line(g.gx(0), g.gy(0), g.gx(cv[0]), g.gy(cv[1]), '#8C96A8');
    line(g.gx(1), g.gy(1), g.gx(cv[2]), g.gy(cv[3]), '#8C96A8');
    var col = hot ? '#E8B04B' : '#AEB6C3', r = Math.max(1.6, g.w / 90);
    for (k = 0; k <= 200; k++) {
      t = k / 200;
      var u = 1 - t, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
      dot(g.gx(b1 * cv[0] + b2 * cv[2] + b3), g.gy(b1 * cv[1] + b2 * cv[3] + b3), r, col);
    }
    var hr = Math.max(6, g.w / 22);
    [[cv[0], cv[1]], [cv[2], cv[3]]].forEach(function (h) {
      dot(g.gx(h[0]), g.gy(h[1]), hr + 2, '#0B0E12');
      dot(g.gx(h[0]), g.gy(h[1]), hr, '#ECE8DF');
    });
  }

  function placeCard() {
    var card = $('card');
    if (card.style.display === 'none') return;
    var app = $('app').getBoundingClientRect(), mk = document.querySelector('.mark.on');
    var cx = app.width / 2;
    if (mk) { var r = mk.getBoundingClientRect(); cx = r.left + r.width / 2 - app.left; }
    var gap = parseFloat(getComputedStyle(document.documentElement).fontSize) * 1.25;
    var left = Math.min(Math.max(cx - card.offsetWidth / 2, gap), app.width - card.offsetWidth - gap);
    card.style.left = Math.round(left) + 'px';
  }

  function setMenu(on) { menuOpen = on; render(false); }

  // ---- input --------------------------------------------------------------------------------------------------------
  function local(field, value) { if (S && S.cams[sel]) S.cams[sel][field] = value; }

  document.addEventListener('click', function (e) {
    if (!S) return;
    if (menuOpen && !e.target.closest('#menu') && !e.target.closest('#btMenu')) setMenu(false);
    var el = e.target.closest('button');
    if (!el) return;
    if (el.id === 'btMenu') { setMenu(!menuOpen); return; }
    if (el.id === 'btBack') { cmd(S.playing && S.rewinding ? 'pause' : 'backwards'); return; }
    if (el.id === 'btSlow') {  // TFCam starts the slow down where the path is paused; the card opens on its options
      var at = +el.dataset.at;
      if (at >= 0) { cmd('slowHere|' + at); sel = at; tab = 'move'; render(true); }
      return;
    }
    if (el.dataset.slow === 'off') { local('slowMo', false); cmd('edit|' + sel + '|slowMo|0'); render(true); return; }
    if (el.id === 'btMore') { expanded = !expanded; render(true); return; }
    if (el.dataset.close === 'card') { sel = -1; render(true); return; }
    if (el.dataset.sel !== undefined) {  // a camera on the timeline: pick it and jump there; again closes the card
      var i = +el.dataset.sel;
      if (i === sel) { sel = -1; render(true); return; }
      sel = i;
      var mk = layout().marks[i];
      if (mk) { scrubAt = mk.t; cmd('seek|' + mk.t.toFixed(2)); }
      render(true); return;
    }
    if (el.dataset.tab) { tab = el.dataset.tab; render(true); return; }
    if (el.dataset.toggle) { var k = el.dataset.toggle; S[k] = !S[k]; cmd(k + '|' + (S[k] ? 1 : 0)); renderMenu(); return; }
    if (el.dataset.cam) { if (sel >= 0) cmd(el.dataset.cam + '|' + sel); return; }
    if (el.dataset.opt) {
      var f = el.dataset.opt, v = +el.dataset.val;
      local(f, v); cmd('edit|' + sel + '|' + f + '|' + v); render(true); return;
    }
    if (el.dataset.preset !== undefined) { setSpeed(el.dataset.preset); return; }
    if (el.id === 'btSceneDel') {   // a second click deletes (as Clear all cameras)
      if (!sceneDelArmed) { sceneDelArmed = true; el.textContent = 'Click again to delete this stage’s path'; setTimeout(function () { sceneDelArmed = false; el.textContent = 'Delete this stage’s path'; }, 3000); return; }
      sceneDelArmed = false; el.textContent = 'Delete this stage’s path'; cmd('sceneDelete'); return;
    }
    if (el.id === 'btClear') {
      if (!clearArmed) { clearArmed = true; el.textContent = 'Click again to clear all ' + S.cams.length + ' cameras'; setTimeout(function () { clearArmed = false; el.textContent = 'Clear all cameras'; }, 3000); return; }
      clearArmed = false; el.textContent = 'Clear all cameras'; sel = -1; cmd('clear'); return;
    }
    if (el.dataset.cmd) {
      var c2 = el.dataset.cmd;
      if (c2 === 'saveAs') { var name = $('saveName').value.trim(); if (name) cmd('saveAs|' + name.replace(/\|/g, ' ')); return; }
      cmd(c2);
    }
  });

  function setSpeed(v) {  // only Reset is left: back to the automatic speed
    var c = S.cams[sel];
    if (!c || v !== 'auto') return;
    c.curveOn = false;
    cmd('edit|' + sel + '|curveOn|0');
    render(true);
  }

  // How long (seconds on screen) or how slow: the slow down keeps where it starts (see slowSpan)
  function slowEdit(c, field, v) {
    var start = slowStart(c), speed = c.slowSpeed, secs = c.time * c.slowLength / Math.sqrt(speed);
    if (field === 'slowHow') speed = v / 100; else secs = v;
    var len = Math.min(Math.max(secs * Math.sqrt(speed) / Math.max(c.time, 0.1), 0.05), Math.max(1 - start, 0.05));
    c.slowMo = true; c.slowCamera = true; c.slowSpeed = speed; c.slowLength = len; c.slowAt = Math.min(start + len / 2, 1);
    return 'edit|' + sel + '|slow|' + c.slowAt.toFixed(3) + '|' + speed.toFixed(3) + '|' + len.toFixed(3);
  }

  document.addEventListener('input', function (e) {
    var el = e.target;
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
    if (field === 'slowFor' || field === 'slowHow') {
      var line = slowEdit(S.cams[sel], field, v);
      throttled('edit-slow', function () { cmd(line); });
      return;
    }
    local(field, f.put(v));
    throttled('edit-' + field, function () { cmd('edit|' + sel + '|' + field + '|' + f.put(v)); });
  });

  document.addEventListener('change', function (e) {
    var el = e.target;
    if (el.id === 'open') { if (el.value) cmd('open|' + el.value); el.value = ''; return; }
    if (el.id === 'watch') { cmd('watch|' + (el.checked ? 1 : 0)); return; }
    if (el.dataset.global) { busy = false; cmd(el.dataset.global + '|' + (el.dataset.global === 'smoothness' ? +el.value / 100 : +el.value)); return; }
    if (!S || !S.cams[sel]) return;
    if (el.dataset.field) {
      var fd = el.dataset.field;
      cmd(fd === 'slowFor' || fd === 'slowHow' ? slowEdit(S.cams[sel], fd, +el.value) : 'edit|' + sel + '|' + fd + '|' + F[fd].put(+el.value));
      busy = false; render(true); return;
    }
    if (el.dataset.flag) {
      var flag = el.dataset.flag, on = el.checked;
      local(flag, on); cmd('edit|' + sel + '|' + flag + '|' + (on ? 1 : 0));
      if (flag === 'zoomTrick' && on && !S.cams[sel].dolly) { local('dolly', 0.3); cmd('edit|' + sel + '|dolly|0.3'); }  // a gentle push in
      render(true); return;
    }
    if (el.dataset.select === 'bone') { local('bone', +el.value); cmd('edit|' + sel + '|bone|' + el.value); render(true); }
  });

  // drags: the timeline (scrub) and the speed curve's handles
  function scrubTo(e) {
    var r = $('track').getBoundingClientRect();
    scrubAt = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)) * layout().total;
    renderTransport();
    throttled('seek', function () { cmd('seek|' + scrubAt.toFixed(2)); });
  }
  function curvePoint(e) {
    var cnv = $('curve'), r = cnv.getBoundingClientRect(), g = curveGeom(cnv);
    var px = (e.clientX - r.left) * cnv.width / r.width, py = (e.clientY - r.top) * cnv.height / r.height;
    return { x: Math.min(1, Math.max(0, (px - g.p) / g.l)), y: Math.min(1, Math.max(0, (g.w - g.p - py) / g.l)), px: px, py: py, g: g };
  }
  document.addEventListener('mousedown', function (e) {
    if (e.button !== 0 || !S) return;
    if (e.target.closest('#track') && !e.target.closest('.mark')) { scrubbing = true; scrubTo(e); e.preventDefault(); return; }
    if (e.target.id === 'curve' && S.cams[sel]) {
      var c = S.cams[sel], cv = curveOf(c), p = curvePoint(e), best = -1, bestD = 1e9;
      for (var k = 0; k < 2; k++) {
        var d = Math.hypot(p.g.gx(cv[k * 2]) - p.px, p.g.gy(cv[k * 2 + 1]) - p.py);
        if (d < bestD) { bestD = d; best = k; }
      }
      if (bestD > p.g.w * 0.15) return;
      if (!c.curveOn) { c.curve = cv.slice(); c.curveOn = true; }
      dragHandle = best; busy = true; e.preventDefault();
    }
  });
  document.addEventListener('mousemove', function (e) {
    if (e.clientX === 0 && e.clientY === 0) return;  // PrismaUI's move to (0,0) when the editor loses focus
    if (scrubbing) { scrubTo(e); return; }
    if (dragHandle < 0 || !S || !S.cams[sel] || !$('curve')) return;
    var c = S.cams[sel], p = curvePoint(e);
    c.curve[dragHandle * 2] = Math.round(p.x * 100) / 100;
    c.curve[dragHandle * 2 + 1] = Math.round(p.y * 100) / 100;
    drawCurve(c.curve, true);
    throttled('curve', function () { cmd('edit|' + sel + '|curve|' + c.curve.join('|')); });
  });
  document.addEventListener('mouseup', function () {
    if (scrubbing) { scrubbing = false; cmd('seek|' + scrubAt.toFixed(2)); return; }
    if (dragHandle >= 0) {
      var c = S && S.cams[sel];
      if (c) cmd('edit|' + sel + '|curve|' + c.curve.map(function (v) { return v.toFixed(3); }).join('|'));
      dragHandle = -1; busy = false; render(true); return;
    }
    if (busy) { busy = false; if (S) render(true); }   // whatever changed meanwhile shows now
  });

  // hover tips: PrismaUI draws no browser tooltips, so the page draws its own - the button's title (or its label),
  // above it, 0.45 s after the pointer rests on it
  var tipEl = null, tipFor = null, tipT = 0;
  function tipText(el) { return el.getAttribute('title') || el.getAttribute('data-tip') || el.getAttribute('aria-label') || ''; }
  function tipHide() { clearTimeout(tipT); tipFor = null; if (tipEl) tipEl.style.display = 'none'; }
  function tipShow(el) {
    var t = tipText(el); if (!t) return;
    if (!tipEl) { tipEl = document.createElement('div'); tipEl.id = 'tip'; document.body.appendChild(tipEl); }
    tipEl.textContent = t; tipEl.style.display = 'block';
    var r = el.getBoundingClientRect(), w = tipEl.offsetWidth, h = tipEl.offsetHeight, gap = 8;
    var x = Math.min(Math.max(r.left + r.width / 2 - w / 2, 6), window.innerWidth - w - 6);
    var y = r.top - h - gap; if (y < 6) y = r.bottom + gap;   // above (the bar is at the bottom), else below
    tipEl.style.left = x + 'px'; tipEl.style.top = y + 'px';
  }
  document.addEventListener('mouseover', function (e) {
    var el = e.target.closest && e.target.closest('button, .mark, select');
    if (el && el.id === 'btInfo') el = null;   // its own panel is the tip
    if (el === tipFor) return;
    tipHide();
    if (!el || !tipText(el)) return;
    // the browser's own tooltip would show too outside the game: the title moves to data-tip
    if (el.getAttribute('title')) { el.setAttribute('data-tip', el.getAttribute('title')); el.removeAttribute('title'); }
    tipFor = el; tipT = setTimeout(function () { if (tipFor === el) tipShow(el); }, 450);
  });
  document.addEventListener('mousedown', tipHide, true);

  // the (i) at the end of the bar: every control and a short guide. Pointing at it shows the panel, moving off both
  // hides it; a click keeps it up until the next click (or Esc)
  var HELP_KEYS = [
    ['While the editor is open', [
      ['Esc / your editor key', 'Close the editor (to fly the free camera)'],
      ['Your Play key', 'Play / pause'],
      ['Your Add camera key', 'Add a camera where the free camera is'],
      ['Your Video key', 'Start / stop recording video'],
      ['Other keys', 'Stay out of the game while the editor is open'],
      ['', 'The keys are set in TFCam\'s settings (Camera Path, Video)']]],
    ['The bar', [
      ['◀  /  ▶', 'Play backwards / play or pause'],
      ['■', 'Stop'],
      ['Snowflake', 'Freeze the world (people, hair, grass); again lets it go'],
      ['Timeline', 'Click or drag to jump along the path'],
      ['Numbered circle', 'Pick that camera and jump there; again closes its card'],
      ['Slow down here', 'Paused part way along a move: slow that part'],
      ['+', 'Add a camera where the free camera is'],
      ['●', 'Record video'],
      ['…', 'The path menu: open, save, loop, follow, smoothness...']]],
    ['A camera\'s card', [
      ['Go here', 'Fly the free camera to this camera'],
      ['Move camera here', 'This camera takes the free camera\'s spot and view'],
      ['More settings', 'Every setting, in the Shot and Move tabs'],
      ['Bin', 'Delete this camera'],
      ['Pick / Me / Console / None', 'The pivot: what the screen centre points at / you / who you click in the console / nothing']]]
  ];
  var HELP_GUIDE = [
    ['Making a path', 'Press Esc, fly the free camera to the first spot, press your Add camera key (or open the editor and press +). Do the same for each spot, then Play. The numbered circles on the timeline are your cameras.'],
    ['Pivot', 'What a camera keeps in view and turns round. Set it in More settings > Shot. Follow moving (in the menu) lets the cameras ride along when the pivot walks.'],
    ['Moves', 'Each camera says how it goes on to the next: Straight, Curve, Orbit (circles the pivot - which way round and extra full turns) or Cut (jumps, like a film cut). Bend left / right and up / down shape a straight move.'],
    ['Speed', 'More settings > Move: drag the round handles on the speed curve - steep is fast, flat is slow; Reset goes back to automatic. To slow just part of a move: play, pause where it should slow, press Slow down here.'],
    ['While a camera waits', 'Wait here holds the camera. Dolly: the camera pushes in or pulls back while the zoom keeps the pivot the same size, so only the background stretches. Shake and Blur the background work there too. Without a pivot, whatever is at your distance is the subject.'],
    ['Saving', 'The menu (…): Save as keeps the path under a name, the list above it opens a saved one. Clear all cameras asks for a second click.'],
    ['SexLab scene paths', 'In a SexLab scene the menu (…) has a SexLab scene part. Save this path for this stage keeps it against the scene, not the world, with your partners as scene positions: that stage plays it wherever the scene runs, looping until you change the stage (the last camera’s move takes it back to camera 1). Normal paths stay where you put them in the world.']
  ];
  var helpPinned = false;
  function helpHtml() {
    var h = '<div class="hcols">';
    HELP_KEYS.forEach(function (sec) {
      h += '<div class="hsec"><div class="cap">' + esc(sec[0]) + '</div>';
      sec[1].forEach(function (r) { h += '<div class="hrow' + (r[0] ? '' : ' note') + '"><b>' + esc(r[0]) + '</b><span>' + esc(r[1]) + '</span></div>'; });
      h += '</div>';
    });
    h += '</div><div class="cap">How things work</div><div class="hguide">';
    HELP_GUIDE.forEach(function (g) { h += '<p><b>' + esc(g[0]) + '.</b> ' + esc(g[1]) + '</p>'; });
    return h + '</div>';
  }
  function showHelp(on) {
    var p = $('help');
    if (on && p.style.display === 'none') p.innerHTML = helpHtml();
    p.style.display = on ? 'block' : 'none';
    $('btInfo').classList.toggle('on', !!on && helpPinned);
  }
  document.addEventListener('mouseover', function (e) {
    if (!e.target.closest) return;
    if (e.target.closest('#btInfo') || e.target.closest('#help')) showHelp(true);
    else if (!helpPinned) showHelp(false);
  });
  document.addEventListener('click', function (e) {
    if (!e.target.closest || !e.target.closest('#btInfo')) return;
    helpPinned = !helpPinned; showHelp(helpPinned);
  }, true);
  document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && helpPinned) { helpPinned = false; showHelp(false); } }, true);

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
    var before = S ? S.cams.length : -1;
    S = st;
    if (!scrubbing && !S.playing) scrubAt = Math.min(scrubAt, S.total || 0);
    // the camera count reaches TFCam.log ("CamPathPrisma page: ...") when it changes - the next test shows where an update stops
    if (S.cams.length !== before) console.info('TFCam editor: ' + S.cams.length + ' camera(s) on the page');
    render(S.cams.length !== before);
  };

  // sizes are in rem: 16 px on a 1920 x 1080 screen, in proportion on others
  function fit() {
    var s = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
    document.documentElement.style.fontSize = (16 * s) + 'px';
  }
  window.addEventListener('resize', function () { fit(); if (S) render(true); });
  fit();

  // ---- outside the game: demo data ---------------------------------------------------------------------------------------
  // #cam picks camera 2 (#first: camera 1, an orbit), #more shows every setting, #menu opens the menu, #many uses 22 cameras, #shot picks
  // that tab, #paused pauses the path half-way from camera 2 to 3 (Slow down here);
  // ?bg=<url> puts a picture behind it.
  function cam(o) {
    var base = { pivot: 'none', pivotKind: -1, bone: 0, look: 1, wait: 0, fov: 70, move: 1, time: 3, seconds: 3, smooth: false, turns: 0, way: 0, wayNow: 0, zoomTrick: false, dolly: 0,
      bendSide: 0, bendUp: 0, blur: false, blurStrength: 0.6, shake: 0, crashZoom: false, slowMo: false, slowAt: 0.66, slowSpeed: 0.4, slowLength: 0.2,
      slowWorld: false, slowCamera: true, elastic: 0, sling: 0, curveOn: false, curve: [0.42, 0, 0.58, 1] };
    for (var k in o) base[k] = o[k];
    return base;
  }
  function previewCmd(s) {
    var a = s.split('|');
    if (a[0] === 'play') { S.playing = !(S.playing && !S.paused && !S.rewinding); S.paused = false; S.rewinding = false; }
    else if (a[0] === 'backwards') { S.playing = true; S.paused = false; S.rewinding = true; }
    else if (a[0] === 'pause') { S.paused = true; S.rewinding = false; }
    else if (a[0] === 'stop') { S.playing = false; }
    else if (a[0] === 'freeze') { S.frozen = !S.frozen; }
    else if (a[0] === 'video') { S.video = !S.video; }
    else if (a[0] === 'seek') { S.playing = false; scrubAt = +a[1]; }
    else if (a[0] === 'add') { S.cams.push(cam({})); S.lastEvent = 'camera ' + S.cams.length + ' added'; }
    else if (a[0] === 'console') { S.lastEvent = 'pivot for camera ' + (+a[1] + 1) + ': Lydia (the console\'s selection)'; S.cams[+a[1]].pivot = 'Lydia'; S.cams[+a[1]].pivotKind = 1; }
    else if (a[0] === 'slowHere') {  // as TFCam does it, from the paused moment (no slow down on that move before)
      var lay = layout(), k = +a[1], c = S.cams[k], sg = lay.segs.filter(function (g) { return g.cam === k && g.kind !== 'wait'; })[0];
      var f = Math.min(Math.max((S.progress * lay.total - sg.t) / c.time, 0), 0.95), len = Math.min(Math.max(1.5 * Math.sqrt(0.3) / c.time, 0.05), 1 - f);
      c.slowMo = true; c.slowCamera = true; c.slowSpeed = 0.3; c.slowLength = len; c.slowAt = f + len / 2;
      c.seconds = c.time * (1 - len + len / Math.sqrt(0.3));
      S.lastEvent = 'camera ' + (k + 1) + ' slows down from here - set how long and how slow';
    }
    else if (a[0] === 'edit') {  // enough for the demo: the slow down's own fields
      var ce = S.cams[+a[1]];
      if (a[2] === 'slow') ce.seconds = ce.time * (1 - ce.slowLength + ce.slowLength / Math.sqrt(ce.slowSpeed));
      if (a[2] === 'slowMo' && a[3] === '0') ce.seconds = ce.time;
    }
    render(false);
  }
  setTimeout(function () {
    if (/Ultralight/.test(navigator.userAgent) || typeof window.tfcamCmd === 'function' || S) { cmd('ready'); return; }
    preview = true;
    var flags = location.hash.slice(1).split(',');
    var has = function (f) { return flags.indexOf(f) >= 0; };
    var bg = /[?&]bg=([^&#]+)/.exec(location.search);
    if (bg) {
      document.body.style.background = '#000 url("' + decodeURIComponent(bg[1]) + '") center / cover no-repeat';
      document.body.style.height = '100vh';
    }
    var badge = document.createElement('div');
    badge.className = 'preview';
    badge.textContent = 'Preview outside the game - demo data';
    document.body.appendChild(badge);
    var cams = [
      cam({ pivot: 'Adrianne', pivotKind: 1, bone: 1, look: 0, wait: 0.2, fov: 62, move: 2, time: 1.9, seconds: 2.2, slowMo: true, curveOn: true, wayNow: 2 }),
      cam({ pivot: 'Adrianne', pivotKind: 1, look: 0, fov: 55, move: 1, time: 3, seconds: 3, bendSide: 0.25, bendUp: 0.1, shake: 0.15, elastic: 0.3, curveOn: true, curve: [0.1, 0.7, 0.3, 1] }),
      cam({ look: 1, wait: 1, move: 3, seconds: 0 }),
      cam({ pivot: 'you', pivotKind: 0, bone: 2, look: 0, wait: 1.5, fov: 40, crashZoom: true })
    ];
    if (has('many')) {
      cams = [];
      var secs = [1.9, 3, 3, 3, 1.7, 0.9, 0.9, 3, 1.5, 3, 4, 1.2, 0.8, 3, 3, 3, 4.5, 3, 2, 6.4, 5, 3];
      secs.forEach(function (sx, k) {
        cams.push(cam({ pivot: k < 2 ? 'Adrianne Avenicci' : 'none', pivotKind: k < 2 ? 1 : -1, move: k % 7 === 6 ? 2 : k % 5 === 4 ? 0 : 1,
          time: sx, seconds: sx, wait: k === 20 ? 5 : k === 21 ? 3 : 0, slowMo: k === 16 }));
      });
    }
    S = {
      name: 'Whiterun - Adrianne', follow: true, loop: false, showPath: true, smoothness: 0.3, aimHeight: 0, playing: false, paused: false, rewinding: false,
      rate: 1, atCam: -1, progress: 0, total: 0, freeCam: !has('off'), frozen: false, clean: false, recordingPath: false, video: false,
      videoStatus: 'not recording', fxStatus: 'idle', fixed: false, watching: false, lastEvent: '', openKey: '1', playKey: '', addKey: '', videoKey: '',
      saved: ['whiterun', 'Before clearing'], cams: cams,
      // #scene = in a SexLab scene (#scene,saved: its stage has a saved scene path)
      sceneIn: has('scene'), sceneLabel: 'Missionary (Billyy) - stage 2', sceneSaved: has('saved'), scenePlaying: false, sceneAuto: true
    };
    S.total = layout().total;
    if (has('cam')) sel = 1;
    if (has('first')) sel = 0;
    if (has('more')) expanded = true;
    if (has('shot')) tab = 'shot';
    if (has('menu')) menuOpen = true;
    scrubAt = sel >= 0 ? layout().marks[sel].t : 2.9;
    if (has('paused')) { S.playing = true; S.paused = true; S.atCam = 1; S.progress = 3.6 / S.total; }  // half-way from 2 to 3
    render(true);
  }, 800);
})();
