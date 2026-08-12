var CHANNEL_LABELS = {
  '': 'Top Rated',
  'bullet': 'Bullet',
  'blitz': 'Blitz',
  'rapid': 'Rapid',
  'classical': 'Classical',
  'chess960': 'Chess960',
  'crazyhouse': 'Crazyhouse',
  'antichess': 'Antichess',
  'atomic': 'Atomic',
  'horde': 'Horde',
  'racingKings': 'Racing Kings',
  'threeCheck': 'Three-check',
  'ultraBullet': 'UltraBullet',
  'bot': 'Bot',
  'computer': 'Computer'
};

var STORAGE_KEY_CHANNEL = 'lichess_tv_channel';
var STORAGE_KEY_THEME = 'lichess_tv_theme';
var STORAGE_KEY_SIZE = 'lichess_tv_size';
var STORAGE_KEY_WRIST_FLICK = 'lichess_tv_wrist_flick';
var STORAGE_KEY_HIGHLIGHT = 'lichess_tv_highlight';
var STORAGE_KEY_LATENCY = 'lichess_tv_latency';
var STORAGE_KEY_INACTIVITY_TIMEOUT = 'lichess_tv_inactivity_timeout';

// Minutes of no button presses on the watch before it auto-exits back to
// the watchface. 0 means "Off" (never auto-exit) and is the default, since
// that matches the app's prior (no-timeout) behavior.
var DEFAULT_INACTIVITY_TIMEOUT_MIN = 0;
var VALID_INACTIVITY_TIMEOUT_VALUES_MIN = [0, 2, 3, 5, 10];

function sanitizeInactivityTimeoutMin(value) {
  var n = parseInt(value, 10);
  return (VALID_INACTIVITY_TIMEOUT_VALUES_MIN.indexOf(n) !== -1) ? n : DEFAULT_INACTIVITY_TIMEOUT_MIN;
}

var DEFAULT_LATENCY_COMPENSATION_MS = 500;
var VALID_LATENCY_VALUES_MS = [250, 500, 750, 1000];

// How much time (ms) to subtract from the actively-ticking player's clock
// before sending it to the watch, to compensate for phone->watch transit
// delay (network + Bluetooth). Applied only to the side whose clock is
// actually running. Updated by the configuration screen; see
// applyLatencyCompensation() below.
var s_latencyCompensationMs = DEFAULT_LATENCY_COMPENSATION_MS;

function sanitizeLatencyMs(value) {
  var n = parseInt(value, 10);
  return (VALID_LATENCY_VALUES_MS.indexOf(n) !== -1) ? n : DEFAULT_LATENCY_COMPENSATION_MS;
}

// Subtracts the configured latency compensation from a clock reading, but
// only for the side whose clock is currently ticking (the other side's
// clock is paused, so there's nothing to compensate for). Clamped at 0.
function applyLatencyCompensation(secs, isActiveSide) {
  if (typeof secs !== 'number' || !isActiveSide || s_latencyCompensationMs <= 0) {
    return secs;
  }
  var adjusted = secs - (s_latencyCompensationMs / 1000);
  return (adjusted < 0) ? 0 : adjusted;
}

var RECONNECT_INTERVAL_MS = 10 * 60 * 1000;
var RETRY_DELAY_MS = 3000;

var STREAM_ALIVE_THRESHOLD_MS = 35000;

var CHANNEL = '';
var FEED_URL = '';
var STATUS_LABEL = '';

function getCheckSquare(board64, activeColor) {
  if (!activeColor || board64.length !== 64) return -1;
  var kingChar = (activeColor === 'w') ? 'K' : 'k';
  var kingIdx = board64.indexOf(kingChar);
  if (kingIdx === -1) return -1;

  var kr = Math.floor(kingIdx / 8), kc = kingIdx % 8;
  var isWhite = (activeColor === 'w');
  var enemyPawn = isWhite ? 'p' : 'P';
  var enemyKnight = isWhite ? 'n' : 'N';
  var enemyRook = isWhite ? 'r' : 'R';
  var enemyBishop = isWhite ? 'b' : 'B';
  var enemyQueen = isWhite ? 'q' : 'Q';

  // Pawns
  var pr = kr + (isWhite ? -1 : 1);
  if (pr >= 0 && pr < 8) {
    if (kc - 1 >= 0 && board64[pr * 8 + (kc - 1)] === enemyPawn) return kingIdx;
    if (kc + 1 < 8  && board64[pr * 8 + (kc + 1)] === enemyPawn) return kingIdx;
  }

  // Knights
  var kOffs = [[-2,-1],[-2,1],[-1,-2],[-1,2],[1,-2],[1,2],[2,-1],[2,1]];
  for (var i = 0; i < kOffs.length; i++) {
    var nr = kr + kOffs[i][0], nc = kc + kOffs[i][1];
    if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
      if (board64[nr * 8 + nc] === enemyKnight) return kingIdx;
    }
  }

  // Sliding Rays (Rooks, Bishops, Queens)
  var dirs = [
    [-1,0,true], [1,0,true], [0,-1,true], [0,1,true],      // Orthogonal
    [-1,-1,false], [-1,1,false], [1,-1,false], [1,1,false] // Diagonal
  ];
  for (var d = 0; d < dirs.length; d++) {
    var dr = dirs[d][0], dc = dirs[d][1], isOrtho = dirs[d][2];
    var r = kr + dr, c = kc + dc;
    while (r >= 0 && r < 8 && c >= 0 && c < 8) {
      var p = board64[r * 8 + c];
      if (p !== '.') {
        if (isOrtho && (p === enemyRook || p === enemyQueen)) return kingIdx;
        if (!isOrtho && (p === enemyBishop || p === enemyQueen)) return kingIdx;
        break;
      }
      r += dr; c += dc;
    }
  }
  return -1;
}

function channelDisplayName(key) {
  return CHANNEL_LABELS.hasOwnProperty(key) ? CHANNEL_LABELS[key] : key;
}

function computeFeedUrlAndLabel() {
  FEED_URL = CHANNEL
    ? 'https://lichess.org/api/tv/' + CHANNEL + '/feed'
    : 'https://lichess.org/api/tv/feed';
  STATUS_LABEL = channelDisplayName(CHANNEL);
}

function expandFenBoard(fenBoardPart) {
  // Crazyhouse FENs append a bracketed pocket directly onto the board part
  // with no separating space (e.g. "...RNBQKBNR[QPn]"). If left in, those
  // pocket characters get counted as extra board squares below, the 64-
  // length check fails, and the board update is silently dropped. Strip it.
  var bracketIdx = fenBoardPart.indexOf('[');
  if (bracketIdx !== -1) {
    fenBoardPart = fenBoardPart.substring(0, bracketIdx);
  }

  var rows = fenBoardPart.split('/');
  if (rows.length !== 8) return null;
  var out = '';
  for (var i = 0; i < 8; i++) {
    var row = rows[i];
    for (var j = 0; j < row.length; j++) {
      var ch = row.charAt(j);
      if (ch === '~') {
        // Marks the preceding piece as "promoted" (crazyhouse); it isn't a
        // square of its own, so skip it rather than counting it as one.
        continue;
      }
      if (ch >= '1' && ch <= '8') {
        var n = parseInt(ch, 10);
        for (var k = 0; k < n; k++) out += '.';
      } else {
        out += ch;
      }
    }
  }
  return (out.length === 64) ? out : null;
}

// Player identity (name/title/rating/patron) is usually only sent once near
// the start of a stream connection; later lines are lightweight deltas
// (fen/lm/wc/bc only). Since extractGameData now only ever sees the newly
// arrived lines for a given call (see handleStreamData), we cache identity
// fields here so they survive across calls without needing to rescan the
// whole cumulative response.
var s_playerCache = {
  whiteTitle: '', whiteName: '', whiteRating: '', whitePatron: false,
  blackTitle: '', blackName: '', blackRating: '', blackPatron: false
};

function resetPlayerCache() {
  s_playerCache.whiteTitle = '';
  s_playerCache.whiteName = '';
  s_playerCache.whiteRating = '';
  s_playerCache.whitePatron = false;
  s_playerCache.blackTitle = '';
  s_playerCache.blackName = '';
  s_playerCache.blackRating = '';
  s_playerCache.blackPatron = false;
}

// `lines` should be only the newly-received lines since the last call
// (see handleStreamData), not the entire cumulative stream buffer.
function extractGameData(lines) {
  var data = {
    fen: null,
    lm: '',
    whiteTitle: s_playerCache.whiteTitle,
    whiteName: s_playerCache.whiteName,
    whiteRating: s_playerCache.whiteRating,
    whiteClockSecs: null,
    whitePatron: s_playerCache.whitePatron,
    blackTitle: s_playerCache.blackTitle,
    blackName: s_playerCache.blackName,
    blackRating: s_playerCache.blackRating,
    blackClockSecs: null,
    blackPatron: s_playerCache.blackPatron
  };

  for (var i = lines.length - 1; i >= 0; i--) {
    var line = lines[i].trim();
    if (!line) continue;
    try {
      var obj = JSON.parse(line);
      var d = obj.d || obj;

      if (!data.fen && d.fen) {
        data.fen = d.fen;
        data.lm = d.lm || '';
      }

      if (d.status) data.status = d.status;
      if (d.winner) data.winner = d.winner;

      if (d.wc !== undefined && data.whiteClockSecs === null) {
        data.whiteClockSecs = d.wc;
      }
      if (d.bc !== undefined && data.blackClockSecs === null) {
        data.blackClockSecs = d.bc;
      }

      if (d.players && Array.isArray(d.players)) {
        d.players.forEach(function(p, idx) {
          var isWhite = (p.color === 'white') || (idx === 0 && !p.color);
          var name = (p.user && p.user.name) ? p.user.name : (p.name || p.id || '');
          var title = (p.user && p.user.title) ? p.user.title : (p.title || '');
          var patron = !!((p.user && p.user.patron) || p.patron);
          var rating = p.rating ? String(p.rating) : '';
          var clockSecs = (p.seconds !== undefined) ? p.seconds : null;

          if (isWhite) {
            if (name) data.whiteName = name;
            if (title) data.whiteTitle = title;
            if (rating) data.whiteRating = rating;
            if (data.whiteClockSecs === null && clockSecs !== null) data.whiteClockSecs = clockSecs;
            if (patron) data.whitePatron = true;
          } else {
            if (name) data.blackName = name;
            if (title) data.blackTitle = title;
            if (rating) data.blackRating = rating;
            if (data.blackClockSecs === null && clockSecs !== null) data.blackClockSecs = clockSecs;
            if (patron) data.blackPatron = true;
          }
        });

        // Persist identity fields immediately so subsequent calls (which
        // may only contain clock/fen deltas with no players array) still
        // have names/titles/ratings available.
        s_playerCache.whiteName = data.whiteName;
        s_playerCache.whiteTitle = data.whiteTitle;
        s_playerCache.whiteRating = data.whiteRating;
        s_playerCache.whitePatron = data.whitePatron;
        s_playerCache.blackName = data.blackName;
        s_playerCache.blackTitle = data.blackTitle;
        s_playerCache.blackRating = data.blackRating;
        s_playerCache.blackPatron = data.blackPatron;
      }

      if (data.fen && data.whiteName && data.blackName) break;
    } catch (e) {
      // Ignore incomplete/invalid JSON chunks (e.g. a line split across
      // two chunk boundaries - handleStreamData holds those back until
      // they're complete).
    }
  }
  return data;
}

function sendBoard(fenBoardPart, statusText, game, activeColor) {
  var expanded = expandFenBoard(fenBoardPart);
  if (!expanded) return;

  var msg = {
    'BoardFEN': expanded,
    'StatusText': statusText,
    'LastMove': (game && game.lm) ? game.lm : '',
    'CheckSquare': getCheckSquare(expanded, activeColor)
  };

  if (game) {
    var whiteSecs = applyLatencyCompensation(game.whiteClockSecs, activeColor === 'w');
    var blackSecs = applyLatencyCompensation(game.blackClockSecs, activeColor === 'b');

    msg['WhiteTitle'] = game.whiteTitle || '';
    msg['WhiteName'] = game.whiteName || 'White';
    msg['WhiteRating'] = game.whiteRating ? ('(' + game.whiteRating + ')') : '';
    msg['WhiteClockSecs'] = (typeof whiteSecs === 'number') ? Math.round(whiteSecs) : -1;
    msg['WhitePatron'] = game.whitePatron ? 1 : 0;
    msg['BlackTitle'] = game.blackTitle || '';
    msg['BlackName'] = game.blackName || 'Black';
    msg['BlackRating'] = game.blackRating ? ('(' + game.blackRating + ')') : '';
    msg['BlackClockSecs'] = (typeof blackSecs === 'number') ? Math.round(blackSecs) : -1;
    msg['BlackPatron'] = game.blackPatron ? 1 : 0;
    msg['ActiveColor'] = activeColor || '';
    var isOutOfTime = (game.status === 'outoftime' || game.status === 31);
    msg['WhiteFlagged'] = (isOutOfTime && game.winner === 'black') || (game.whiteClockSecs === 0 && game.winner === 'black') ? 1 : 0;
    msg['BlackFlagged'] = (isOutOfTime && game.winner === 'white') || (game.blackClockSecs === 0 && game.winner === 'white') ? 1 : 0;
  }

  Pebble.sendAppMessage(
    msg,
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: sendAppMessage failed: ' + JSON.stringify(err));
    }
  );
}

function sendStatusOnly(statusText) {
  Pebble.sendAppMessage(
    { 'StatusText': statusText },
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: status send failed: ' + JSON.stringify(err));
    }
  );
}

function sendStreamAliveReply() {
  var alive = !!(s_xhr && s_xhr.readyState === 3 &&
    (Date.now() - s_lastDataReceivedAt) < STREAM_ALIVE_THRESHOLD_MS);

  var msg = { 'StreamAlive': alive ? 1 : 0 };

  if (alive) {
    msg['StatusText'] = STATUS_LABEL + ' \u2022 Lichess TV \u2022 live';
    var whiteSecs = applyLatencyCompensation(s_prevWhiteSecs, s_activeColor === 'w');
    var blackSecs = applyLatencyCompensation(s_prevBlackSecs, s_activeColor === 'b');
    msg['WhiteClockSecs'] = (typeof whiteSecs === 'number') ? Math.round(whiteSecs) : -1;
    msg['BlackClockSecs'] = (typeof blackSecs === 'number') ? Math.round(blackSecs) : -1;
    msg['ActiveColor'] = s_activeColor || '';
  }

  Pebble.sendAppMessage(
    msg,
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: stream-alive reply failed: ' + JSON.stringify(err));
    }
  );
}

var s_xhr = null;
var s_lastFen = null;
var s_reconnectTimer = null;
var s_periodicResetTimer = null;

var s_lastPingSentAt = 0;
// The watch (STALE_TIMEOUT_MS in lichess-tv.c) only needs *some* message
// within 20s to avoid triggering a CheckStream round trip. 12s leaves a
// comfortable margin while cutting background BLE traffic significantly
// compared to the previous 4s interval.
var KEEPALIVE_PING_MIN_INTERVAL_MS = 12000;

var s_prevWhiteSecs = null;
var s_prevBlackSecs = null;
var s_activeColor = '';

var s_lastDataReceivedAt = 0;

// xhr.responseText on readyState 3 returns the *entire* response received so
// far, not just the new chunk. To avoid re-splitting/re-parsing an
// ever-growing buffer on every incoming line, we track how much of it we've
// already consumed and only process the newly-arrived suffix. Any trailing
// partial line (split across chunk boundaries) is held in s_streamLineBuffer
// until it's completed by a later chunk.
var s_lastProcessedLength = 0;
var s_streamLineBuffer = '';

function clearReconnectTimer() {
  if (s_reconnectTimer) {
    clearTimeout(s_reconnectTimer);
    s_reconnectTimer = null;
  }
}

function scheduleReconnect(delayMs) {
  clearReconnectTimer();
  s_reconnectTimer = setTimeout(connectStream, delayMs);
}

function updateActiveColor(game) {
  if (game && game.fen) {
    var parts = game.fen.split(' ');
    if (parts.length > 1 && (parts[1] === 'w' || parts[1] === 'b')) {
      s_activeColor = parts[1];
      s_prevWhiteSecs = (typeof game.whiteClockSecs === 'number') ? game.whiteClockSecs : s_prevWhiteSecs;
      s_prevBlackSecs = (typeof game.blackClockSecs === 'number') ? game.blackClockSecs : s_prevBlackSecs;
      return s_activeColor;
    }
  }

  var newWhite = (typeof game.whiteClockSecs === 'number') ? game.whiteClockSecs : null;
  var newBlack = (typeof game.blackClockSecs === 'number') ? game.blackClockSecs : null;

  if (newWhite === null || newBlack === null) {
    s_activeColor = '';
  } else if (s_prevWhiteSecs !== null && s_prevBlackSecs !== null) {
    if (newWhite < s_prevWhiteSecs) {
      s_activeColor = 'b';
    } else if (newBlack < s_prevBlackSecs) {
      s_activeColor = 'w';
    }
  }

  s_prevWhiteSecs = newWhite;
  s_prevBlackSecs = newBlack;
  return s_activeColor;
}

function handleStreamData(xhr) {
  s_lastDataReceivedAt = Date.now();

  var text = xhr.responseText || '';
  var newChunk = (text.length > s_lastProcessedLength) ? text.substring(s_lastProcessedLength) : '';
  s_lastProcessedLength = text.length;
  if (!newChunk) return;

  var combined = s_streamLineBuffer + newChunk;
  var lines = combined.split('\n');
  // The last element may be an incomplete line if this chunk ended
  // mid-line; hold it back and prepend it to the next chunk.
  s_streamLineBuffer = lines.pop();

  var game = extractGameData(lines);

  // Refresh active-color/clock tracking on every update, not just when the
  // fen changes, so a stream-alive check (sendStreamAliveReply) never sends
  // clock values that are stale from several updates ago.
  var activeColor = updateActiveColor(game);

  if (game.fen && game.fen !== s_lastFen) {
    s_lastFen = game.fen;
    var boardPart = game.fen.split(' ')[0];
    sendBoard(boardPart, STATUS_LABEL + ' \u2022 Lichess TV \u2022 live', game, activeColor);
    s_lastPingSentAt = Date.now();
    return;
  }

  var now = Date.now();
  if (now - s_lastPingSentAt >= KEEPALIVE_PING_MIN_INTERVAL_MS) {
    s_lastPingSentAt = now;
    sendStatusOnly(STATUS_LABEL + ' \u2022 Lichess TV \u2022 live');
  }
}

function connectStream() {
  clearReconnectTimer();
  if (s_periodicResetTimer) {
    clearTimeout(s_periodicResetTimer);
    s_periodicResetTimer = null;
  }
  if (s_xhr) {
    try {
      s_xhr.onreadystatechange = null;
      s_xhr.abort();
    } catch (e) {}
  }

  s_lastProcessedLength = 0;
  s_streamLineBuffer = '';

  console.log('lichess-tv: connecting stream ' + FEED_URL);
  var xhr = new XMLHttpRequest();
  s_xhr = xhr;

  xhr.onreadystatechange = function () {
    if (xhr !== s_xhr) return;
    if (xhr.readyState === 3) {
      handleStreamData(xhr);
    } else if (xhr.readyState === 4) {
      handleStreamData(xhr);
      console.log('lichess-tv: stream closed, reconnecting shortly');
      scheduleReconnect(RETRY_DELAY_MS);
    }
  };

  xhr.onerror = function () {
    if (xhr !== s_xhr) return;
    console.log('lichess-tv: stream error, reconnecting shortly');
    scheduleReconnect(RETRY_DELAY_MS);
  };

  try {
    xhr.open('GET', FEED_URL, true);
    xhr.send();
  } catch (e) {
    console.log('lichess-tv: stream open/send failed: ' + e);
    scheduleReconnect(RETRY_DELAY_MS);
    return;
  }

  s_periodicResetTimer = setTimeout(connectStream, RECONNECT_INTERVAL_MS);
}

function sendChannelSwitching(statusText) {
  Pebble.sendAppMessage(
    {
      'StatusText': statusText,
      'WhiteClockSecs': -1,
      'BlackClockSecs': -1,
      'ActiveColor': '',
      'LastMove': '',
      'CheckSquare': -1
    },
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: channel-switch status send failed: ' + JSON.stringify(err));
    }
  );
}

function setChannel(newChannel) {
  if (!CHANNEL_LABELS.hasOwnProperty(newChannel)) return;
  if (newChannel === CHANNEL && FEED_URL) return;

  CHANNEL = newChannel;
  computeFeedUrlAndLabel();
  s_lastFen = null;
  s_lastPingSentAt = 0;
  s_prevWhiteSecs = null;
  s_prevBlackSecs = null;
  s_activeColor = '';
  resetPlayerCache();

  try {
    localStorage.setItem(STORAGE_KEY_CHANNEL, CHANNEL);
  } catch (e) {}

  sendChannelSwitching(STATUS_LABEL + ' \u2022 connecting...');
  console.log('lichess-tv: switching to channel "' + CHANNEL + '"');
  connectStream();
}

Pebble.addEventListener('appmessage', function (e) {
  var payload = e.payload || {};
  if (typeof payload.SelectedChannel !== 'undefined') {
    setChannel(payload.SelectedChannel);
  }
  if (typeof payload.CheckStream !== 'undefined') {
    sendStreamAliveReply();
  }
});

Pebble.addEventListener('showConfiguration', function () {
  var savedTheme = localStorage.getItem(STORAGE_KEY_THEME) || '0';
  var savedSize = localStorage.getItem(STORAGE_KEY_SIZE) || '0';
  var savedWristFlick = localStorage.getItem(STORAGE_KEY_WRIST_FLICK) || '1';
  var savedHighlight = localStorage.getItem(STORAGE_KEY_HIGHLIGHT) || '4';
  var savedLatency = String(sanitizeLatencyMs(localStorage.getItem(STORAGE_KEY_LATENCY) || DEFAULT_LATENCY_COMPENSATION_MS));
  var savedInactivityTimeout = String(sanitizeInactivityTimeoutMin(localStorage.getItem(STORAGE_KEY_INACTIVITY_TIMEOUT)));

  var html = '<!DOCTYPE html><html><head>' +
  '<meta charset="utf-8">' +
  '<meta name="viewport" content="width=device-width, initial-scale=1">' +
  '<style>' +
  '* { box-sizing: border-box; }' +
  'body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; padding: 20px; background: #161512; color: #bababa; margin: 0; }' +
  '.page-header { display: flex; align-items: center; gap: 10px; border-bottom: 1px solid #262421; padding-bottom: 14px; margin-bottom: 22px; }' +
  '.page-header .mark { width: 10px; height: 10px; border-radius: 3px; background: #629924; flex-shrink: 0; }' +
  '.page-header h2 { margin: 0; color: #e2e2e2; font-size: 19px; font-weight: 600; }' +

  '.section { background: #1c1a17; border: 1px solid #2a2824; border-radius: 10px; margin-bottom: 18px; overflow: hidden; }' +
  '.section-header { padding: 13px 16px 10px; }' +
  '.section-header .title { color: #d3d3d3; font-size: 12px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; }' +
  '.section-header .desc { color: #6e6b66; font-size: 12px; margin-top: 2px; }' +

  '.field { padding: 12px 16px 14px; }' +
  '.field + .field { border-top: 1px solid #262421; }' +
  'label { display: block; font-weight: 600; margin-bottom: 8px; color: #959595; font-size: 12px; letter-spacing: 0.2px; }' +
  'select { width: 100%; padding: 12px; font-size: 16px; border-radius: 4px; border: 1px solid #363431; background: #262421; color: #e2e2e2; outline: none; -webkit-appearance: none; }' +
  'select:focus { border-color: #3692e7; }' +

  'button { width: 100%; padding: 14px; font-size: 16px; background: #629924; color: #ffffff; border: none; border-radius: 4px; font-weight: bold; margin-top: 4px; cursor: pointer; transition: background 0.15s ease; }' +
  'button:active { background: #52801e; }' +
  '</style></head><body>' +

  '<div class="page-header"><span class="mark"></span><h2>Lichess TV Settings</h2></div>' +

  '<div class="section">' +
  '<div class="section-header"><div class="title">Board</div><div class="desc">Appearance while a game is live</div></div>' +
  '<div class="field"><label>Board Theme</label>' +
  '<select id="theme">' +
  '<option value="0">Brown (Default)</option>' +
  '<option value="1">Blue2</option>' +
  '<option value="2">Green</option>' +
  '<option value="3">Olive</option>' +
  '<option value="4">Gray</option>' +
  '<option value="5">Purple</option>' +
  '<option value="6">Purple-Diag</option>' +
  '<option value="7">Leather</option>' +
  '<option value="8">Pink</option>' +
  '</select></div>' +
  '<div class="field"><label>Board Size</label>' +
  '<select id="size">' +
  '<option value="0">Big</option>' +
  '<option value="1">Biggest (Edge to Edge)</option>' +
  '</select></div>' +
  '<div class="field"><label>Last Move Highlight</label>' +
  '<select id="highlight">' +
  '<option value="0">Off</option>' +
  '<option value="1">Blue</option>' +
  '<option value="2">Blue2</option>' +
  '<option value="3">Green</option>' +
  '<option value="4">Green2</option>' +
  '</select></div>' +
  '<div class="field"><label>Wrist Flick to Flip Board</label>' +
  '<select id="wristFlick">' +
  '<option value="0">Off</option>' +
  '<option value="1">On</option>' +
  '</select></div>' +
  '</div>' +

  '<div class="section">' +
  '<div class="section-header"><div class="title">Connection &amp; Power</div><div class="desc">Clock accuracy and battery life</div></div>' +
  '<div class="field"><label>Latency Compensation</label>' +
  '<select id="latency">' +
  '<option value="250">250 ms</option>' +
  '<option value="500">500 ms (Default)</option>' +
  '<option value="750">750 ms</option>' +
  '<option value="1000">1000 ms</option>' +
  '</select></div>' +
  '<div class="field"><label>Inactivity Timeout</label>' +
  '<select id="inactivityTimeout">' +
  '<option value="0">Off (Default)</option>' +
  '<option value="2">2 minutes</option>' +
  '<option value="3">3 minutes</option>' +
  '<option value="5">5 minutes</option>' +
  '<option value="10">10 minutes</option>' +
  '</select></div>' +
  '</div>' +

  '<button onclick="save()">Save Settings</button>' +
  '<script>' +
  'document.getElementById("theme").value = "' + savedTheme + '";' +
  'document.getElementById("size").value = "' + savedSize + '";' +
  'document.getElementById("wristFlick").value = "' + savedWristFlick + '";' +
  'document.getElementById("highlight").value = "' + savedHighlight + '";' +
  'document.getElementById("latency").value = "' + savedLatency + '";' +
  'document.getElementById("inactivityTimeout").value = "' + savedInactivityTimeout + '";' +
  'function save() {' +
  '  var theme = parseInt(document.getElementById("theme").value, 10);' +
  '  var size = parseInt(document.getElementById("size").value, 10);' +
  '  var wristFlick = parseInt(document.getElementById("wristFlick").value, 10);' +
  '  var highlight = parseInt(document.getElementById("highlight").value, 10);' +
  '  var latency = parseInt(document.getElementById("latency").value, 10);' +
  '  var inactivityTimeout = parseInt(document.getElementById("inactivityTimeout").value, 10);' +
  '  var return_to = new URLSearchParams(window.location.search).get("return_to") || "pebblejs://close#";' +
  '  window.location.href = return_to + encodeURIComponent(JSON.stringify({ theme: theme, size: size, wristFlick: wristFlick, highlight: highlight, latency: latency, inactivityTimeout: inactivityTimeout }));' +
  '}' +
  '</script></body></html>';

  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e.response) return;

  try {
    var config = JSON.parse(decodeURIComponent(e.response));
    var theme = parseInt(config.theme, 10);
    var size = parseInt(config.size, 10);
    var wristFlick = parseInt(config.wristFlick, 10) || 0;
    var highlight = parseInt(config.highlight, 10) || 0;
    var latency = sanitizeLatencyMs(config.latency);
    var inactivityTimeout = sanitizeInactivityTimeoutMin(config.inactivityTimeout);

    localStorage.setItem(STORAGE_KEY_THEME, theme);
    localStorage.setItem(STORAGE_KEY_SIZE, size);
    localStorage.setItem(STORAGE_KEY_WRIST_FLICK, wristFlick);
    localStorage.setItem(STORAGE_KEY_HIGHLIGHT, highlight);
    localStorage.setItem(STORAGE_KEY_LATENCY, latency);
    localStorage.setItem(STORAGE_KEY_INACTIVITY_TIMEOUT, inactivityTimeout);

    s_latencyCompensationMs = latency;

    Pebble.sendAppMessage({
      'BoardTheme': theme,
      'BoardSize': size,
      'WristFlickEnabled': wristFlick,
      'BoardHighlight': highlight,
      'InactivityTimeoutMin': inactivityTimeout
    });
  } catch (err) {
    console.log('lichess-tv: failed to parse config response: ' + err);
  }
});

Pebble.addEventListener('ready', function () {
  try {
    var savedChannel = localStorage.getItem(STORAGE_KEY_CHANNEL);
    if (savedChannel !== null && CHANNEL_LABELS.hasOwnProperty(savedChannel)) {
      CHANNEL = savedChannel;
    }
  } catch (e) {}

  var savedTheme = parseInt(localStorage.getItem(STORAGE_KEY_THEME) || '0', 10);
  var savedSize = parseInt(localStorage.getItem(STORAGE_KEY_SIZE) || '0', 10);
  var savedWristFlick = parseInt(localStorage.getItem(STORAGE_KEY_WRIST_FLICK) || '0', 10);
  var savedHighlight = parseInt(localStorage.getItem(STORAGE_KEY_HIGHLIGHT) || '0', 10);
  s_latencyCompensationMs = sanitizeLatencyMs(localStorage.getItem(STORAGE_KEY_LATENCY) || DEFAULT_LATENCY_COMPENSATION_MS);
  var savedInactivityTimeout = sanitizeInactivityTimeoutMin(localStorage.getItem(STORAGE_KEY_INACTIVITY_TIMEOUT));

  computeFeedUrlAndLabel();
  console.log('lichess-tv: pkjs ready, connecting to ' + FEED_URL);

  Pebble.sendAppMessage(
    {
      'SelectedChannel': CHANNEL,
      'BoardTheme': savedTheme,
      'BoardSize': savedSize,
      'WristFlickEnabled': savedWristFlick,
      'BoardHighlight': savedHighlight,
      'InactivityTimeoutMin': savedInactivityTimeout
    },
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: initial sync failed: ' + JSON.stringify(err));
    }
  );

  connectStream();
});