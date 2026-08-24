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

// Remembers the last tournament (broadcast) game the user was watching, so
// the app can jump straight back to it on next launch instead of always
// falling back to the last TV channel. Mutually exclusive with
// STORAGE_KEY_CHANNEL as "the thing to resume": setChannel() clears these
// whenever the user explicitly picks a TV channel instead, and
// switchToBroadcastGame() writes them whenever a broadcast game is
// selected (whether by hand or by this same restore path). See
// restoreLastBroadcastGame() near the 'ready' handler.
var STORAGE_KEY_BROADCAST_ROUND = 'lichess_tv_broadcast_round';
var STORAGE_KEY_BROADCAST_GAME = 'lichess_tv_broadcast_game';
var STORAGE_KEY_BROADCAST_LABEL = 'lichess_tv_broadcast_label';

// Same idea as the broadcast-resume keys above, but for the last streamer
// the user selected (STREAM_MODE_GAME) - also mutually exclusive with both
// STORAGE_KEY_CHANNEL and the broadcast keys as "the thing to resume".
// switchToStreamerGame() writes STORAGE_KEY_STREAMER on every explicit
// selection (whether or not the streamer turns out to be live right now),
// and setChannel()/switchToBroadcastGame() clear it when the user picks
// something else instead. STORAGE_KEY_STREAMER_SNAPSHOT caches the last
// board/clocks/names actually seen for that streamer so both a cold
// restart and a "they're currently offline" state have something real to
// show immediately, instead of a blank board - see
// sendCachedStreamerSnapshot() and saveStreamerSnapshot() near the
// "Live streamers" section below.
var STORAGE_KEY_STREAMER = 'lichess_tv_streamer';
var STORAGE_KEY_STREAMER_SNAPSHOT = 'lichess_tv_streamer_snapshot';

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

// --- Lichess Broadcasts -------------------------------------------------
//
// STREAM_MODE picks which of two totally different feeds connectStream()
// is following. TV mode gets a stream of NDJSON lines with a ready-made
// FEN per update (see extractGameData). Broadcasts don't work that way:
// the broadcast round stream (https://lichess.org/api/stream/broadcast/
// round/{id}.pgn) is plain PGN text, NOT NDJSON - confirmed by directly
// reading the raw response (an earlier version of this file guessed
// NDJSON from stale docs and was wrong). It's one tag per line, a blank
// line, then movetext, games back-to-back - same shape as the static
// /api/broadcast/round/{id}.pgn snapshot, so splitPgnGames/pgnTag/
// pgnMovetext all apply directly. The initial connection dumps every
// game in the round this way; each later move-update re-appends that
// one game's block (same tags, fresh movetext) rather than resending
// the whole round. Getting a board out of a block still means replaying
// its SAN moves ourselves - see the mini move-applier below
// (newStartingBoard64 onward).
//
// Because a game's block can appear more than once (initial dump, then
// again per update) with no positional index tying a given occurrence
// back to "the Nth game we listed", we always take the LAST block whose
// White/Black tags match TARGET_WHITE/TARGET_BLACK (captured from the
// static round PGN when the game was selected - see
// fetchRoundGames/s_roundGameNames below) rather than relying on
// position.
var STREAM_MODE_TV = 0;
var STREAM_MODE_BROADCAST = 1;
// A single streamer's current game, streamed via /api/stream/game/{id} -
// see the "Live streamers" section further down for why this needs its
// own STREAM_MODE (the gameFull/gameState NDJSON envelope with UCI moves
// is a third, different shape from both TV's per-update FEN and
// broadcast's PGN blocks).
var STREAM_MODE_GAME = 2;
var STREAM_MODE = STREAM_MODE_TV;

var MAX_BROADCASTS = 20;
var MAX_BROADCAST_GAMES = 12;

var BROADCAST_ROUND_ID = '';
var BROADCAST_GAME_INDEX = -1; // this game's position among ALL games in the round PGN
var BROADCAST_LABEL = '';
var TARGET_WHITE = '';         // White/Black tags of the selected game, used to pick its
var TARGET_BLACK = '';         // updates out of the round's per-game stream (see above)
var s_broadcastNameById = {};   // round ID -> display label, filled by fetchBroadcastList
var s_roundGameNames = [];      // index (matches pgn_index) -> { white, black }, filled by fetchRoundGames
var s_lastBroadcastBoard = null;

var MAX_STREAMERS = 20;
var TARGET_STREAMER_NAME = ''; // username of the streamer currently being watched (STREAM_MODE_GAME)

// --- Move review support ----------------------------------------------
//
// The watch keeps its own move history and handles UP/DOWN navigation
// locally (no round trip needed) and never clears it just because a new
// game starts - the buffer simply keeps flowing across game boundaries.
// This file's job is just to compute a short move label per move (string
// work is much easier here than in C) and send it as "LastMoveSAN"
// alongside the normal board update.
//
// New-game detection is still needed here, though: without it, the first
// board update of a new game would get diffed against the previous game's
// final position and produce a nonsensical move label. Prefers the feed's
// own game id when a line happens to carry one; falls back to a
// piece-count heuristic below since not every feed line includes it.
var s_currentGameId = null;

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
  if (STREAM_MODE === STREAM_MODE_BROADCAST) {
    FEED_URL = 'https://lichess.org/api/stream/broadcast/round/' + BROADCAST_ROUND_ID + '.pgn';
    STATUS_LABEL = BROADCAST_LABEL || 'Broadcast';
    return;
  }
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

function pieceCount(board64) {
  var n = 0;
  for (var i = 0; i < board64.length; i++) {
    if (board64.charAt(i) !== '.') n++;
  }
  return n;
}

function algebraicToIndex(sq) {
  if (!sq || sq.length < 2) return -1;
  var col = sq.charCodeAt(0) - 'a'.charCodeAt(0);
  var row = 8 - parseInt(sq.charAt(1), 10);
  if (col < 0 || col > 7 || isNaN(row) || row < 0 || row > 7) return -1;
  return row * 8 + col;
}

function indexToAlgebraic(idx) {
  var row = Math.floor(idx / 8), col = idx % 8;
  return String.fromCharCode('a'.charCodeAt(0) + col) + (8 - row);
}

// Builds a best-effort, short move label (e.g. "e4", "Nf3", "Bxc6", "O-O",
// "e8=Q") from the previous/next board and lichess's UCI-style "lm" field
// (e.g. "e2e4", or "e7e8q" for promotion). This is intentionally simplified
// for a watch-sized display: it does NOT disambiguate two identical pieces
// that could both reach the same square (e.g. "Nbd2" vs "Nfd2" both just
// show as "Nd2"), and it does not append "+"/"#" for check/checkmate. Given
// the small screen this is a reasonable trade-off, not a rules engine.
function shortMoveLabel(prevBoard64, newBoard64, lm) {
  if (!prevBoard64 || !newBoard64 || !lm || lm.length < 4) return lm || '';

  var fromIdx = algebraicToIndex(lm.substring(0, 2));
  var toIdx = algebraicToIndex(lm.substring(2, 4));
  if (fromIdx < 0 || toIdx < 0) return lm;

  var movedChar = prevBoard64.charAt(fromIdx); // piece as it was before moving
  if (movedChar === '.' || movedChar === undefined) return lm;

  var pieceType = movedChar.toUpperCase();
  var isPawn = (pieceType === 'P');
  var fromCol = fromIdx % 8;
  var toCol = toIdx % 8;

  // Castling: king moving two files.
  if (pieceType === 'K' && Math.abs(toCol - fromCol) === 2) {
    return (toCol > fromCol) ? 'O-O' : 'O-O-O';
  }

  var destSquare = indexToAlgebraic(toIdx);
  var wasOccupied = prevBoard64.charAt(toIdx) !== '.';
  // En passant: pawn moves diagonally into an empty square.
  var isEnPassant = isPawn && (fromCol !== toCol) && !wasOccupied;
  var isCapture = wasOccupied || isEnPassant;

  var promotion = (lm.length >= 5) ? lm.charAt(4).toUpperCase() : '';

  var label;
  if (isPawn) {
    var fromFile = String.fromCharCode('a'.charCodeAt(0) + fromCol);
    label = isCapture ? (fromFile + 'x' + destSquare) : destSquare;
    if (promotion) label += '=' + promotion;
  } else {
    label = pieceType + (isCapture ? 'x' : '') + destSquare;
  }
  return label;
}

// --- Broadcast PGN parsing / SAN replay ---------------------------------
//
// The broadcast round PGN has no live FEN, so this section rebuilds a
// board64 the same way the TV feed's FEN already comes pre-built: replay
// every SAN move from the standard starting position. This intentionally
// skips full legality checking (pins, discovered checks) - it only needs
// to find a piece of the right type, obeying the SAN's own file/rank
// disambiguation, that can geometrically reach the destination square.
// Real games' own SAN disambiguation almost always leaves exactly one
// candidate anyway, so this is a reasonable trade-off for a watch app,
// in the same spirit as shortMoveLabel's simplifications above.

// Splits a multi-game PGN blob into individual game blocks, each starting
// at its own "[Event " tag line.
function splitPgnGames(pgnText) {
  var parts = pgnText.split(/(?=\[Event )/);
  var games = [];
  for (var i = 0; i < parts.length; i++) {
    if (parts[i].indexOf('[Event ') === 0) games.push(parts[i]);
  }
  return games;
}

function pgnTag(block, tag) {
  var m = block.match(new RegExp('\\[' + tag + ' "([^"]*)"'));
  return m ? m[1] : '';
}

// Everything after the tag section (the last contiguous run of lines
// starting with '[') is the movetext.
function pgnMovetext(block) {
  var lines = block.split('\n');
  var idx = 0;
  while (idx < lines.length && (lines[idx].charAt(0) === '[' || lines[idx].trim() === '')) idx++;
  return lines.slice(idx).join(' ');
}

// Tokenizes movetext into an ordered list of { san, clk } - clk (seconds,
// or null) comes from a "{ [%clk H:MM:SS] }" comment immediately following
// the move it applies to. Move numbers, NAGs, and the trailing result
// marker are all dropped.
function tokenizeMovetext(movetext) {
  var text = movetext.replace(/\$\d+/g, ' ');
  var re = /([^\s{}]+)|\{([^}]*)\}/g;
  var moves = [];
  var m;
  while ((m = re.exec(text)) !== null) {
    if (m[1] !== undefined) {
      var tok = m[1];
      if (/^\d+\.+$/.test(tok)) continue;
      if (tok === '*' || tok === '1-0' || tok === '0-1' || tok === '1/2-1/2') continue;
      moves.push({ san: tok, clk: null });
    } else if (m[2] !== undefined && moves.length > 0) {
      var clkMatch = m[2].match(/%clk\s+(\d+):(\d+):(\d+)/);
      if (clkMatch) {
        moves[moves.length - 1].clk =
          parseInt(clkMatch[1], 10) * 3600 + parseInt(clkMatch[2], 10) * 60 + parseInt(clkMatch[3], 10);
      }
    }
  }
  return moves;
}

function newStartingBoard64() {
  return 'rnbqkbnr' + 'pppppppp' + '........' + '........' +
         '........' + '........' + 'PPPPPPPP' + 'RNBQKBNR';
}

function clearPath(board64, fr, fc, tr, tc) {
  var dr = (tr === fr) ? 0 : (tr > fr ? 1 : -1);
  var dc = (tc === fc) ? 0 : (tc > fc ? 1 : -1);
  var r = fr + dr, c = fc + dc;
  while (r !== tr || c !== tc) {
    if (board64.charAt(r * 8 + c) !== '.') return false;
    r += dr; c += dc;
  }
  return true;
}
function knightReach(fr, fc, tr, tc) {
  var dr = Math.abs(fr - tr), dc = Math.abs(fc - tc);
  return (dr === 1 && dc === 2) || (dr === 2 && dc === 1);
}
function kingReach(fr, fc, tr, tc) {
  return Math.max(Math.abs(fr - tr), Math.abs(fc - tc)) === 1;
}
function rookReach(board64, fr, fc, tr, tc) {
  return (fr === tr || fc === tc) && clearPath(board64, fr, fc, tr, tc);
}
function bishopReach(board64, fr, fc, tr, tc) {
  return (Math.abs(fr - tr) === Math.abs(fc - tc)) && clearPath(board64, fr, fc, tr, tc);
}
function queenReach(board64, fr, fc, tr, tc) {
  return rookReach(board64, fr, fc, tr, tc) || bishopReach(board64, fr, fc, tr, tc);
}

// Applies one SAN token to board64 for the given side to move. Returns
// { board, uci } on success, or null if the move couldn't be parsed/found
// (the caller stops the replay there rather than guessing further).
function applySanMove(board64, sideToMove, sanRaw) {
  var san = (sanRaw || '').replace(/[+#!?]+$/, '');
  var isWhite = (sideToMove === 'w');
  var arr = board64.split('');

  if (san === 'O-O' || san === '0-0') {
    var rank = isWhite ? 7 : 0;
    var kf = rank * 8 + 4, kt = rank * 8 + 6, rf = rank * 8 + 7, rt = rank * 8 + 5;
    arr[kt] = arr[kf]; arr[kf] = '.';
    arr[rt] = arr[rf]; arr[rf] = '.';
    return { board: arr.join(''), uci: indexToAlgebraic(kf) + indexToAlgebraic(kt) };
  }
  if (san === 'O-O-O' || san === '0-0-0') {
    var rank2 = isWhite ? 7 : 0;
    var kf2 = rank2 * 8 + 4, kt2 = rank2 * 8 + 2, rf2 = rank2 * 8 + 0, rt2 = rank2 * 8 + 3;
    arr[kt2] = arr[kf2]; arr[kf2] = '.';
    arr[rt2] = arr[rf2]; arr[rf2] = '.';
    return { board: arr.join(''), uci: indexToAlgebraic(kf2) + indexToAlgebraic(kt2) };
  }

  var m = san.match(/^([KQRBN]?)([a-h]?)([1-8]?)(x?)([a-h][1-8])(=?([QRBN]))?$/);
  if (!m) return null;

  var pieceLetter = m[1] || 'P';
  var fromFile = m[2] || null;
  var fromRank = m[3] || null;
  var isCapture = (m[4] === 'x');
  var destSq = m[5];
  var promo = m[7] || null;

  var destIdx = algebraicToIndex(destSq);
  if (destIdx < 0) return null;
  var destR = Math.floor(destIdx / 8), destC = destIdx % 8;
  var pieceChar = isWhite ? pieceLetter : pieceLetter.toLowerCase();

  var srcIdx = -1;

  if (pieceLetter === 'P') {
    var pawnDir = isWhite ? -1 : 1; // white pawns move toward row 0 (rank 8)
    if (isCapture) {
      var srcFile = fromFile ? (fromFile.charCodeAt(0) - 'a'.charCodeAt(0)) : destC;
      srcIdx = (destR - pawnDir) * 8 + srcFile;
    } else {
      var oneBack = (destR - pawnDir) * 8 + destC;
      srcIdx = (arr[oneBack] === pieceChar) ? oneBack : (destR - 2 * pawnDir) * 8 + destC;
    }
  } else {
    for (var i = 0; i < 64; i++) {
      if (arr[i] !== pieceChar) continue;
      var r = Math.floor(i / 8), c = i % 8;
      if (fromFile && c !== (fromFile.charCodeAt(0) - 'a'.charCodeAt(0))) continue;
      if (fromRank && r !== (8 - parseInt(fromRank, 10))) continue;
      var reach = false;
      if (pieceLetter === 'N') reach = knightReach(r, c, destR, destC);
      else if (pieceLetter === 'B') reach = bishopReach(board64, r, c, destR, destC);
      else if (pieceLetter === 'R') reach = rookReach(board64, r, c, destR, destC);
      else if (pieceLetter === 'Q') reach = queenReach(board64, r, c, destR, destC);
      else if (pieceLetter === 'K') reach = kingReach(r, c, destR, destC);
      if (reach) { srcIdx = i; break; }
    }
  }
  if (srcIdx < 0 || srcIdx > 63 || arr[srcIdx] !== pieceChar) return null;

  // En passant: a pawn capture landing on an empty square.
  if (pieceLetter === 'P' && isCapture && arr[destIdx] === '.') {
    var capturedIdx = Math.floor(srcIdx / 8) * 8 + destC;
    arr[capturedIdx] = '.';
  }

  var movingChar = arr[srcIdx];
  arr[srcIdx] = '.';
  arr[destIdx] = promo ? (isWhite ? promo : promo.toLowerCase()) : movingChar;

  return {
    board: arr.join(''),
    uci: indexToAlgebraic(srcIdx) + indexToAlgebraic(destIdx) + (promo ? promo.toLowerCase() : '')
  };
}

// Replays an entire move list from the standard starting position.
// Stops (keeping the last good position) if a move fails to parse/apply,
// rather than producing a corrupted board.
function replayGameToBoard(moves) {
  var board = newStartingBoard64();
  var side = 'w';
  var lastUci = '';
  for (var i = 0; i < moves.length; i++) {
    var res = applySanMove(board, side, moves[i].san);
    if (!res) break;
    board = res.board;
    lastUci = res.uci;
    side = (side === 'w') ? 'b' : 'w';
  }
  return { board: board, activeColor: side, lastUci: lastUci };
}

function resetGameTracking() {
  s_currentGameId = null;
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

// --- Head-to-head score (Lichess crosstable) ----------------------------
//
// https://lichess.org/api/crosstable/{user1}/{user2} returns each player's
// aggregate score across every game they've played against each other
// (win = 1, draw = 0.5, loss = 0), e.g. { users: { magnus: 3, hikaru: 1.5 },
// nbGames: 5 }. That's exactly "player score" next to the rating: for a
// pair who've only ever met once and that meeting was a win, the winner's
// score is 1 (nbGames: 1) and a draw is 0.5 for both.
//
// Scores are formatted as a decimal whenever they're not a whole number
// (0.5 -> "0.5", 1.5 -> "1.5", 2.5 -> "2.5", ...) and as a plain integer
// otherwise ("1", "2", "3"...).
var s_crosstableCache = {};    // "white|black" (lowercased) -> { white: "1", black: "0" } | null
var s_crosstablePending = {};  // same key -> true while a fetch is in flight
var s_activeScoreWhite = '';   // whichever pairing's score is currently on screen -
var s_activeScoreBlack = '';   // used to ignore a crosstable response that arrives after
                                // the game/channel has already moved on.

function crosstableKey(whiteName, blackName) {
  return (whiteName || '').toLowerCase() + '|' + (blackName || '').toLowerCase();
}

function formatCrosstableScore(score) {
  if (typeof score !== 'number' || isNaN(score)) return '';
  var doubled = Math.round(score * 2);
  if (doubled < 0) return '';
  return String(doubled / 2);
}

function sendScoreUpdate(whiteScore, blackScore) {
  Pebble.sendAppMessage(
    { 'WhiteScore': whiteScore || '', 'BlackScore': blackScore || '' },
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: score send failed: ' + JSON.stringify(err));
    }
  );
}

function fetchCrosstable(whiteName, blackName, key) {
  if (s_crosstablePending[key]) return;
  s_crosstablePending[key] = true;

  var url = 'https://lichess.org/api/crosstable/' + encodeURIComponent(whiteName) + '/' + encodeURIComponent(blackName);
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;
    delete s_crosstablePending[key];
    if (xhr.status !== 200) return;

    try {
      var obj = JSON.parse(xhr.responseText || '{}');
      if (!obj || !obj.users || !obj.nbGames) {
        s_crosstableCache[key] = null;
        return;
      }
      var wScore = obj.users[whiteName.toLowerCase()];
      var bScore = obj.users[blackName.toLowerCase()];
      if (typeof wScore !== 'number' || typeof bScore !== 'number') {
        s_crosstableCache[key] = null;
        return;
      }

      var entry = { white: formatCrosstableScore(wScore), black: formatCrosstableScore(bScore) };
      s_crosstableCache[key] = entry;

      // Only push it to the watch if this is still the pairing on screen -
      // the user may have switched games/channels while the request was
      // in flight.
      if (key === crosstableKey(s_activeScoreWhite, s_activeScoreBlack)) {
        sendScoreUpdate(entry.white, entry.black);
      }
    } catch (e) {
      console.log('lichess-tv: crosstable parse failed: ' + e);
    }
  };
  xhr.onerror = function () {
    delete s_crosstablePending[key];
    console.log('lichess-tv: crosstable fetch failed');
  };
  try {
    xhr.open('GET', url, true);
    xhr.send();
  } catch (e) {
    delete s_crosstablePending[key];
    console.log('lichess-tv: crosstable open failed: ' + e);
  }
}

// Called whenever the current game's player names are known. No-ops if the
// pairing hasn't actually changed and forceRefresh isn't set (so it's cheap
// to call on every stream update, not just once per game). Pass
// forceRefresh=true when a new game has started between the same two
// players (e.g. a match or arena where they keep meeting) - otherwise the
// pairing-unchanged check above would skip the refetch entirely and the
// badge would keep showing the score from before the most recent game.
function updateHeadToHeadScore(whiteName, blackName, forceRefresh) {
  whiteName = whiteName || '';
  blackName = blackName || '';
  var pairingChanged = (whiteName !== s_activeScoreWhite || blackName !== s_activeScoreBlack);
  if (!pairingChanged && !forceRefresh) return;

  s_activeScoreWhite = whiteName;
  s_activeScoreBlack = blackName;

  if (!whiteName || !blackName) {
    sendScoreUpdate('', '');
    return;
  }

  var key = crosstableKey(whiteName, blackName);
  if (forceRefresh) {
    // Drop any cached value so the stale-but-still-cached score doesn't
    // get resent as-is - force an actual re-fetch to pick up the game
    // that just finished.
    delete s_crosstableCache[key];
  }

  var cached = s_crosstableCache[key];
  if (cached) {
    sendScoreUpdate(cached.white, cached.black);
  } else {
    if (pairingChanged) {
      // Clear the old badge immediately - the new one streams in async once
      // the fetch resolves - rather than briefly showing the previous
      // pairing's score against the new players. For a same-pairing
      // refresh (forceRefresh with the same two players) keep showing the
      // last known score until the fresh value arrives instead of
      // blanking the badge between games.
      sendScoreUpdate('', '');
    }
    fetchCrosstable(whiteName, blackName, key);
  }
}

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
    gameId: null,
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

      if (!data.gameId && d.id) {
        data.gameId = d.id;
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

function sendBoard(fenBoardPart, statusText, game, activeColor, extra) {
  var expanded = expandFenBoard(fenBoardPart);
  if (!expanded) return;
  sendBoardCore(expanded, statusText, game, activeColor, extra);
}

// Same as sendBoard, but for callers (broadcast mode) that already have a
// ready-made board64 instead of a compact FEN board part to expand.
function sendBoardDirect(board64, statusText, game, activeColor, extra) {
  if (!board64 || board64.length !== 64) return;
  sendBoardCore(board64, statusText, game, activeColor, extra);
}

// Tracks the full set of board-related fields we believe the watch
// currently holds, so sendBoardCore can send only what actually changed
// instead of the whole ~15-field dictionary on every single move -
// AppMessage round trips over Bluetooth aren't free, and most fields
// (names, ratings, titles, patron flags...) only ever change once per
// game. Anything that sends board/clock/status fields to the watch
// *outside* of sendBoardCore (sendChannelSwitching, sendStatusOnly, the
// alive branch of sendStreamAliveReply) invalidates this via
// invalidateBoardSendCache() so the next sendBoardCore call can't
// wrongly assume a field is already showing what it last computed here.
var s_lastSentBoardMsg = null;

function invalidateBoardSendCache() {
  s_lastSentBoardMsg = null;
}

function sendBoardCore(expanded, statusText, game, activeColor, extra) {
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

  if (extra) {
    for (var key in extra) {
      if (extra.hasOwnProperty(key)) {
        msg[key] = extra[key];
      }
    }
  }

  // The watch's in_recv_handler already treats every field as optional
  // (it looks each one up with dict_find and simply skips whatever isn't
  // present), so it's safe to omit anything that hasn't actually changed
  // since the last full picture we sent - this is by far the biggest,
  // most frequent message the app sends, so trimming it matters more
  // than any other single change here.
  var toSend = msg;
  if (s_lastSentBoardMsg) {
    var diff = {};
    var anyChanged = false;
    for (var key2 in msg) {
      if (!msg.hasOwnProperty(key2)) continue;
      if (s_lastSentBoardMsg[key2] !== msg[key2]) {
        diff[key2] = msg[key2];
        anyChanged = true;
      }
    }
    s_lastSentBoardMsg = msg;
    if (!anyChanged) return; // nothing actually changed - skip the round trip entirely
    toSend = diff;
  } else {
    s_lastSentBoardMsg = msg;
  }

  Pebble.sendAppMessage(
    toSend,
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: sendAppMessage failed: ' + JSON.stringify(err));
    }
  );
}

function sendStatusOnly(statusText) {
  invalidateBoardSendCache();
  Pebble.sendAppMessage(
    { 'StatusText': statusText },
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: status send failed: ' + JSON.stringify(err));
    }
  );
}

function sendStreamAliveReply() {
  var xhrAlive = !!(s_xhr && s_xhr.readyState === 3 &&
    (Date.now() - s_lastDataReceivedAt) < STREAM_ALIVE_THRESHOLD_MS);

  var msg;
  if (xhrAlive) {
    invalidateBoardSendCache();
    var modeLabel = (STREAM_MODE === STREAM_MODE_BROADCAST) ? 'Broadcast' : 'Lichess TV';
    var whiteSecs = applyLatencyCompensation(s_prevWhiteSecs, s_activeColor === 'w');
    var blackSecs = applyLatencyCompensation(s_prevBlackSecs, s_activeColor === 'b');
    msg = {
      'StreamAlive': 1,
      'StatusText': STATUS_LABEL + ' \u2022 ' + modeLabel + ' \u2022 live',
      'WhiteClockSecs': (typeof whiteSecs === 'number') ? Math.round(whiteSecs) : -1,
      'BlackClockSecs': (typeof blackSecs === 'number') ? Math.round(blackSecs) : -1,
      'ActiveColor': s_activeColor || ''
    };
  } else {
    // No live stream open right now - that's expected (not "lost") while
    // we're deliberately idle waiting for an offline streamer's next
    // game (see scheduleStreamerRecheck). Report alive without touching
    // StatusText/clocks/board so their last known position keeps showing
    // undisturbed rather than getting stomped by "feed lost, retrying...".
    msg = { 'StreamAlive': s_streamerWaitingOffline ? 1 : 0 };
  }

  Pebble.sendAppMessage(
    msg,
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: stream-alive reply failed: ' + JSON.stringify(err));
    }
  );
}

function boardPartOf(fenFull) {
  return fenFull ? fenFull.split(' ')[0] : null;
}

// Detects a new game starting on the current channel (as opposed to a plain
// move within the same game) so the SAN-label diff below doesn't compare a
// new game's board against the previous game's final position and produce
// a nonsensical label. Prefers the feed's own game id when a line happens
// to carry one; falls back to a piece-count heuristic (a real game's piece
// count can only stay the same or shrink move-to-move, never grow) since
// not every line in the feed includes an id.
function detectAndHandleNewGame(game, newBoardPart) {
  if (game.gameId && s_currentGameId && game.gameId !== s_currentGameId) {
    s_currentGameId = game.gameId;
    resetGameTracking();
    s_lastFen = null;
    return true;
  }
  if (game.gameId && !s_currentGameId) {
    s_currentGameId = game.gameId;
  }

  var prevBoardPart = boardPartOf(s_lastFen);
  if (prevBoardPart) {
    var prevExpanded = expandFenBoard(prevBoardPart);
    var newExpanded = expandFenBoard(newBoardPart);
    if (prevExpanded && newExpanded && pieceCount(newExpanded) > pieceCount(prevExpanded)) {
      resetGameTracking();
      s_lastFen = null;
      return true;
    }
  }
  return false;
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

// Dispatches to whichever parser matches the currently-connected feed -
// see the STREAM_MODE comment near the top of the file for why these two
// need genuinely different parsing.
function handleStreamData(xhr) {
  if (STREAM_MODE === STREAM_MODE_BROADCAST) {
    handleBroadcastStreamData(xhr);
  } else if (STREAM_MODE === STREAM_MODE_GAME) {
    handleGameStreamData(xhr);
  } else {
    handleTvStreamData(xhr);
  }
}

// Confirmed by actually reading the raw stream (thank you for running the
// test script) - this is NOT nd-json. It's plain PGN text: one tag per
// line, a blank line, then the movetext line(s), games back-to-back with
// no separator beyond a new "[Event " line starting the next block. The
// initial connection gets every game in the round this way, and each
// later move-update re-appends that one game's block (same tags, fresh
// movetext) rather than resending the whole round. So splitPgnGames on
// the full cumulative text (same helper the static-snapshot fetch uses)
// is exactly the right tool - we just need the *last* block whose
// White/Black tags match our selected game, since a game's block can
// appear more than once as it's updated.
function handleBroadcastStreamData(xhr) {
  s_lastDataReceivedAt = Date.now();

  var text = xhr.responseText || '';
  if (!text || BROADCAST_GAME_INDEX < 0) return;

  var blocks = splitPgnGames(text);
  var block = null;
  var blockWhite = '', blockBlack = '';
  for (var i = blocks.length - 1; i >= 0; i--) {
    var w = pgnTag(blocks[i], 'White');
    var b = pgnTag(blocks[i], 'Black');
    if ((!TARGET_WHITE || w === TARGET_WHITE) && (!TARGET_BLACK || b === TARGET_BLACK)) {
      block = blocks[i];
      blockWhite = w;
      blockBlack = b;
      break;
    }
  }
  if (!block) return;

  var result = pgnTag(block, 'Result') || '*';
  var moves = tokenizeMovetext(pgnMovetext(block));
  var replay = replayGameToBoard(moves);

  if (replay.board === s_lastBroadcastBoard) {
    var idleNow = Date.now();
    if (idleNow - s_lastPingSentAt >= KEEPALIVE_PING_MIN_INTERVAL_MS) {
      s_lastPingSentAt = idleNow;
      sendStatusOnly(STATUS_LABEL + ' \u2022 Broadcast \u2022 live');
    }
    return;
  }

  var extra = {};
  if (s_lastBroadcastBoard && replay.lastUci) {
    extra['LastMoveSAN'] = shortMoveLabel(s_lastBroadcastBoard, replay.board, replay.lastUci);
  }
  s_lastBroadcastBoard = replay.board;

  var whiteClockSecs = null, blackClockSecs = null;
  for (var mi = moves.length - 1; mi >= 0 && (whiteClockSecs === null || blackClockSecs === null); mi--) {
    if (moves[mi].clk === null) continue;
    if (mi % 2 === 0 && whiteClockSecs === null) whiteClockSecs = moves[mi].clk;
    if (mi % 2 === 1 && blackClockSecs === null) blackClockSecs = moves[mi].clk;
  }

  s_activeColor = replay.activeColor;
  s_prevWhiteSecs = (typeof whiteClockSecs === 'number') ? whiteClockSecs : s_prevWhiteSecs;
  s_prevBlackSecs = (typeof blackClockSecs === 'number') ? blackClockSecs : s_prevBlackSecs;

  updateHeadToHeadScore(blockWhite, blockBlack);

  var game = {
    lm: replay.lastUci,
    whiteName: blockWhite || 'White',
    blackName: blockBlack || 'Black',
    whiteRating: pgnTag(block, 'WhiteElo'),
    blackRating: pgnTag(block, 'BlackElo'),
    whiteClockSecs: whiteClockSecs,
    blackClockSecs: blackClockSecs,
    whiteTitle: '', blackTitle: '', whitePatron: false, blackPatron: false
  };

  var statusText = (result === '*')
    ? (STATUS_LABEL + ' \u2022 Broadcast \u2022 live')
    : (STATUS_LABEL + ' \u2022 ' + result);

  sendBoardDirect(replay.board, statusText, game, s_activeColor, extra);
  s_lastPingSentAt = Date.now();
}

// --- Streamer's current game ---------------------------------------------
//
// https://lichess.org/api/stream/game/{id} streams real-time snapshots
// of an ongoing game as NDJSON. VERIFIED against a real live game (not
// guessed): the very first line is game metadata - {"id","variant",
// "speed","perf","rated","source","createdAt","players":{"white":
// {"name","rating","title"},"black":{...}}} - with no "fen" - and every
// line after that is a plain position delta - {"fen","lm","wc","bc"} -
// where "lm" and the clocks may be absent on the very first delta (it's
// just "here's where things stand now", not a move that just happened).
// This is close enough to the TV channel feed's own shape that it's
// rendered with the same FEN-based sendBoard() the TV feed uses, rather
// than any move-replay logic.
//
// Player identity only arrives once, on that first line, so - exactly
// like s_playerCache does for the TV feed - it has to be cached rather
// than re-read from every update.
var s_gameStreamWhiteName = '';
var s_gameStreamBlackName = '';
var s_gameStreamWhiteRating = '';
var s_gameStreamBlackRating = '';
var s_gameStreamWhiteTitle = '';
var s_gameStreamBlackTitle = '';
var s_lastGameStreamFen = null;
var s_streamerRecheckTimer = null;

// True whenever we're deliberately sitting on a streamer's game with no
// live stream actually open - either because they're not currently
// playing (showing their last known position instead, see
// sendCachedStreamerSnapshot below) or a lookup is in flight. Read by
// sendStreamAliveReply() so the watch's own stale-stream watchdog doesn't
// mistake "quietly waiting for them to start a game" for a broken
// connection and stomp the display with "feed lost, retrying...".
var s_streamerWaitingOffline = false;

function resetGameStreamState() {
  s_gameStreamWhiteName = '';
  s_gameStreamBlackName = '';
  s_gameStreamWhiteRating = '';
  s_gameStreamBlackRating = '';
  s_gameStreamWhiteTitle = '';
  s_gameStreamBlackTitle = '';
  s_lastGameStreamFen = null;
  if (s_streamerRecheckTimer) {
    clearTimeout(s_streamerRecheckTimer);
    s_streamerRecheckTimer = null;
  }
}

// Persists the last board/clocks/names actually seen for the streamer
// currently being watched, so a cold restart (see restoreLastStreamer)
// or a "not playing right now" lookup result (see switchToStreamerGame)
// has something real to show instead of a blank board.
function saveStreamerSnapshot(fen, game, lastMoveSan) {
  if (!TARGET_STREAMER_NAME) return;
  try {
    localStorage.setItem(STORAGE_KEY_STREAMER_SNAPSHOT, JSON.stringify({
      u: TARGET_STREAMER_NAME,
      fen: fen,
      lm: lastMoveSan || '',
      wn: game.whiteName, bn: game.blackName,
      wr: game.whiteRating, br: game.blackRating,
      wt: game.whiteTitle, bt: game.blackTitle,
      wc: game.whiteClockSecs, bc: game.blackClockSecs
    }));
  } catch (e) {}
}

// Sends the cached snapshot for `username`, if any, so the watch shows
// their last known position/times instead of a blank board while a
// lookup is in flight or while they're offline. Sent with no active
// color (rather than whatever side was to move when it was captured) so
// the watch's local per-second clock ticker never counts down a clock
// that isn't really running - this is a frozen, past position, not a
// live one. Returns true if a snapshot was actually sent.
function sendCachedStreamerSnapshot(username) {
  var raw;
  try { raw = localStorage.getItem(STORAGE_KEY_STREAMER_SNAPSHOT); } catch (e) { return false; }
  if (!raw) return false;

  var snap;
  try { snap = JSON.parse(raw); } catch (e) { return false; }
  if (!snap || snap.u !== username || !snap.fen) return false;

  s_gameStreamWhiteName = snap.wn || '';
  s_gameStreamBlackName = snap.bn || '';
  s_gameStreamWhiteRating = snap.wr || '';
  s_gameStreamBlackRating = snap.br || '';
  s_gameStreamWhiteTitle = snap.wt || '';
  s_gameStreamBlackTitle = snap.bt || '';
  s_lastGameStreamFen = snap.fen;
  s_activeColor = '';

  var game = {
    lm: snap.lm || '',
    whiteName: s_gameStreamWhiteName || 'White',
    blackName: s_gameStreamBlackName || 'Black',
    whiteRating: s_gameStreamWhiteRating,
    blackRating: s_gameStreamBlackRating,
    whiteClockSecs: (typeof snap.wc === 'number') ? snap.wc : null,
    blackClockSecs: (typeof snap.bc === 'number') ? snap.bc : null,
    whiteTitle: s_gameStreamWhiteTitle,
    blackTitle: s_gameStreamBlackTitle,
    whitePatron: false, blackPatron: false
  };

  sendBoard(snap.fen.split(' ')[0], username + ' \u2022 Streamer \u2022 last game', game, '', {});
  return true;
}

// Keeps checking whether the streamer has started a game, roughly every
// 15s, for as long as they remain the selected target - mirroring how the
// TV channel feed itself moves on to a new pairing once a game ends, but
// without ever switching the watch away to something else in the
// meantime (see switchToStreamerGame's isOngoing===false branch).
function scheduleStreamerRecheck() {
  if (s_streamerRecheckTimer || !TARGET_STREAMER_NAME) return;
  s_streamerWaitingOffline = true;
  s_streamerRecheckTimer = setTimeout(function () {
    s_streamerRecheckTimer = null;
    if (TARGET_STREAMER_NAME) {
      switchToStreamerGame(TARGET_STREAMER_NAME, true);
    }
  }, 15000);
}

function applyGameStreamLine(obj) {
  if (obj.players) {
    var white = obj.players.white || {};
    var black = obj.players.black || {};
    // Defensive: read both the flat shape confirmed by a real response
    // (players.white.name) and a nested players.white.user.name shape,
    // in case it varies per game (e.g. anonymous/bot players).
    var whiteUser = white.user || {};
    var blackUser = black.user || {};
    var whiteName = white.name || whiteUser.name || white.id || whiteUser.id;
    var blackName = black.name || blackUser.name || black.id || blackUser.id;
    var whiteRating = (typeof white.rating === 'number') ? white.rating : whiteUser.rating;
    var blackRating = (typeof black.rating === 'number') ? black.rating : blackUser.rating;
    var whiteTitle = white.title || whiteUser.title;
    var blackTitle = black.title || blackUser.title;

    if (!whiteName || !blackName) {
      console.log('lichess-tv: streamer game line had a "players" object but no usable name - raw: ' + JSON.stringify(obj.players));
    }

    if (whiteName) s_gameStreamWhiteName = whiteName;
    if (blackName) s_gameStreamBlackName = blackName;
    if (typeof whiteRating === 'number') s_gameStreamWhiteRating = String(whiteRating);
    if (typeof blackRating === 'number') s_gameStreamBlackRating = String(blackRating);
    if (whiteTitle) s_gameStreamWhiteTitle = whiteTitle;
    if (blackTitle) s_gameStreamBlackTitle = blackTitle;
    if (s_gameStreamWhiteName && s_gameStreamBlackName) {
      updateHeadToHeadScore(s_gameStreamWhiteName, s_gameStreamBlackName);
    }
  }

  // A terminal line (if one ever arrives - unconfirmed, so this is
  // opportunistic, not relied on exclusively; see the "stream closed"
  // fallback in connectStream's onreadystatechange too) carries a
  // "status" other than "started"/"created" once the game is over. Once
  // that's true the clock has genuinely stopped, so any board sent below
  // must go out with no active color - otherwise the watch's local
  // per-second clock ticker would keep counting down a clock that isn't
  // actually running anymore.
  var isTerminal = !!(obj.status && obj.status !== 'started' && obj.status !== 'created');
  if (isTerminal) {
    scheduleStreamerRecheck();
  }

  if (!obj.fen) return;
  if (obj.fen === s_lastGameStreamFen) {
    var idleNow = Date.now();
    if (idleNow - s_lastPingSentAt >= KEEPALIVE_PING_MIN_INTERVAL_MS) {
      s_lastPingSentAt = idleNow;
      sendStatusOnly(STATUS_LABEL + ' \u2022 Streamer \u2022 live');
    }
    return;
  }

  var boardPart = obj.fen.split(' ')[0];
  var activeColor = isTerminal ? '' : ((obj.fen.split(' ')[1] === 'b') ? 'b' : 'w');

  var extra = {};
  if (s_lastGameStreamFen && obj.lm) {
    var prevBoardPart = s_lastGameStreamFen.split(' ')[0];
    var prevExpanded = expandFenBoard(prevBoardPart);
    var newExpanded = expandFenBoard(boardPart);
    if (prevExpanded && newExpanded) {
      extra['LastMoveSAN'] = shortMoveLabel(prevExpanded, newExpanded, obj.lm);
    }
  }
  s_lastGameStreamFen = obj.fen;
  s_activeColor = activeColor;

  if (!s_gameStreamWhiteName || !s_gameStreamBlackName) {
    console.log('lichess-tv: sending streamer board with no cached player names yet - this line\'s keys: ' + JSON.stringify(Object.keys(obj)));
  }

  var game = {
    lm: obj.lm || '',
    whiteName: s_gameStreamWhiteName || 'White',
    blackName: s_gameStreamBlackName || 'Black',
    whiteRating: s_gameStreamWhiteRating,
    blackRating: s_gameStreamBlackRating,
    whiteClockSecs: (typeof obj.wc === 'number') ? obj.wc : null,
    blackClockSecs: (typeof obj.bc === 'number') ? obj.bc : null,
    whiteTitle: s_gameStreamWhiteTitle,
    blackTitle: s_gameStreamBlackTitle,
    whitePatron: false, blackPatron: false
  };

  saveStreamerSnapshot(obj.fen, game, extra.LastMoveSAN);

  var statusText = STATUS_LABEL + ' \u2022 Streamer \u2022 ' + (isTerminal ? 'game over' : 'live');
  sendBoard(boardPart, statusText, game, activeColor, extra);
  s_lastPingSentAt = Date.now();
}

function handleGameStreamData(xhr) {
  s_lastDataReceivedAt = Date.now();

  var text = xhr.responseText || '';
  var newChunk = (text.length > s_lastProcessedLength) ? text.substring(s_lastProcessedLength) : '';
  s_lastProcessedLength = text.length;
  if (!newChunk) return;

  var combined = s_streamLineBuffer + newChunk;
  var lines = combined.split('\n');
  s_streamLineBuffer = lines.pop();

  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    if (!line) continue;
    var obj;
    try { obj = JSON.parse(line); } catch (e) { continue; }
    applyGameStreamLine(obj);
  }
}

function handleTvStreamData(xhr) {
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

  // Detect a new game before touching the score, so a fresh game between
  // the very same two players (a match/arena where they keep meeting)
  // still forces a crosstable re-fetch instead of no-op'ing on an
  // unchanged pairing (see updateHeadToHeadScore's forceRefresh param).
  var isNewGame = false;
  if (game.fen) {
    isNewGame = detectAndHandleNewGame(game, game.fen.split(' ')[0]);
  }

  updateHeadToHeadScore(game.whiteName, game.blackName, isNewGame);

  if (game.fen) {
    var boardPart = game.fen.split(' ')[0];

    if (game.fen !== s_lastFen) {
      var extra = {};
      var prevBoardPart = boardPartOf(s_lastFen);
      if (prevBoardPart && game.lm) {
        var prevExpanded = expandFenBoard(prevBoardPart);
        var newExpanded = expandFenBoard(boardPart);
        if (prevExpanded && newExpanded) {
          extra['LastMoveSAN'] = shortMoveLabel(prevExpanded, newExpanded, game.lm);
        }
      }

      s_lastFen = game.fen;
      sendBoard(boardPart, STATUS_LABEL + ' \u2022 Lichess TV \u2022 live', game, activeColor, extra);
      s_lastPingSentAt = Date.now();
      return;
    }
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
      if (STREAM_MODE === STREAM_MODE_GAME) {
        // A closed single-game stream almost always means the game
        // ended (Lichess doesn't keep it open indefinitely afterwards).
        // Reconnecting to the same now-finished game's URL would just
        // reopen a stream that never updates again, so re-look-up the
        // streamer's current game instead of blindly reconnecting - this
        // is the fallback for scheduleStreamerRecheck in case a terminal
        // "status" line is never actually sent (unconfirmed either way).
        scheduleStreamerRecheck();
      } else {
        scheduleReconnect(RETRY_DELAY_MS);
      }
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

function sendChannelSwitching(statusText, isBroadcast, isStreamer) {
  invalidateBoardSendCache();
  Pebble.sendAppMessage(
    {
      'StatusText': statusText,
      'WhiteClockSecs': -1,
      'BlackClockSecs': -1,
      'ActiveColor': '',
      'LastMove': '',
      'CheckSquare': -1,
      'BroadcastActive': isBroadcast ? 1 : 0,
      'StreamerActive': isStreamer ? 1 : 0
    },
    function () { /* delivered */ },
    function (err) {
      console.log('lichess-tv: channel-switch status send failed: ' + JSON.stringify(err));
    }
  );
}

// Pebble's outbox effectively serializes sends anyway, but chaining
// explicitly (next item only sent once the previous one is acked/failed)
// is what actually guarantees delivery order, which matters here since
// the watch is building an indexed list out of these one by one.
function sendQueuedMessages(msgs, onDone) {
  var i = 0;
  function sendNext() {
    if (i >= msgs.length) { if (onDone) onDone(); return; }
    var msg = msgs[i++];
    Pebble.sendAppMessage(msg, sendNext, function (err) {
      console.log('lichess-tv: queued send failed: ' + JSON.stringify(err));
      sendNext();
    });
  }
  sendNext();
}

// Fetches https://lichess.org/api/broadcast (one JSON object per line, each
// a full tournament plus every one of its rounds) and reports back up to
// MAX_BROADCASTS entries for whichever tournaments currently have a round
// flagged "ongoing" - a tournament with no ongoing round is skipped
// entirely rather than falling back to a finished one, since this list is
// specifically "what's live right now".
//
// Two different things can make a tournament look duplicated here, so this
// guards against both:
//   1. The exact same round ID appears more than once in the feed (seen in
//      practice for multi-round tournaments - each round's line still
//      carries that tour's *entire* round list, so "which ongoing round do
//      we pick" resolves to the same round ID every time). seenRoundIds
//      below skips any repeat of a round ID outright, unconditionally.
//   2. A single real-world event can be several separate broadcast entries
//      (different boards/sections) that all share one tour.name but do
//      each have a genuinely distinct round ID - "only disambiguate with
//      the round name when there's more than one round" isn't enough to
//      tell those apart, since each may only have a single round of its
//      own. So the round's own name is always folded in whenever it adds
//      anything tour.name doesn't already say, and as a final safety net,
//      any labels that still collide get numbered ("Name", "Name (2)",
//      "Name (3)"...) so two genuinely different broadcasts never render
//      as indistinguishable rows.
function fetchBroadcastList() {
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;

    var items = [];
    var seenRoundIds = {};
    var labelCounts = {};
    try {
      var lines = (xhr.responseText || '').split('\n');
      for (var i = 0; i < lines.length && items.length < MAX_BROADCASTS; i++) {
        var line = lines[i].trim();
        if (!line) continue;
        var obj;
        try { obj = JSON.parse(line); } catch (e) { continue; }

        var tour = obj.tour || {};
        var rounds = obj.rounds || (obj.round ? [obj.round] : []);

        var chosen = null;
        for (var r = 0; r < rounds.length; r++) {
          if (rounds[r].ongoing) { chosen = rounds[r]; break; }
        }
        if (!chosen || !chosen.id) continue;
        if (seenRoundIds[chosen.id]) continue;
        seenRoundIds[chosen.id] = true;

        var label = tour.name || 'Broadcast';
        if (chosen.name && label.indexOf(chosen.name) === -1) {
          label += ' - ' + chosen.name;
        }

        var count = (labelCounts[label] || 0) + 1;
        labelCounts[label] = count;
        var displayLabel = (count > 1) ? (label + ' (' + count + ')') : label;

        items.push({ id: chosen.id, name: displayLabel });
      }
    } catch (e) {
      console.log('lichess-tv: broadcast list parse failed: ' + e);
    }

    var msgs = [];
    for (var idx = 0; idx < items.length; idx++) {
      s_broadcastNameById[items[idx].id] = items[idx].name;
      msgs.push({ 'BroadcastIndex': idx, 'BroadcastId': items[idx].id, 'BroadcastName': items[idx].name });
    }
    msgs.push({ 'BroadcastListDone': 1, 'BroadcastCount': items.length });
    sendQueuedMessages(msgs);
  };
  xhr.onerror = function () {
    console.log('lichess-tv: broadcast list fetch failed');
    Pebble.sendAppMessage({ 'BroadcastListDone': 1, 'BroadcastCount': 0 });
  };
  try {
    xhr.open('GET', 'https://lichess.org/api/broadcast?nb=50', true);
    xhr.send();
  } catch (e) {
    console.log('lichess-tv: broadcast list open failed: ' + e);
    Pebble.sendAppMessage({ 'BroadcastListDone': 1, 'BroadcastCount': 0 });
  }
}

// Fetches the selected round's PGN (https://lichess.org/api/broadcast/
// round/{roundId}.pgn - the plain, non-streaming snapshot, so ordinary
// splitPgnGames parsing applies here) and reports back up to
// MAX_BROADCAST_GAMES games that don't yet have a final Result (i.e. are
// still live) - each tagged with its position among ALL games in the
// PGN. That position travels back out as MESSAGE_KEY_SelectedGame once
// picked, purely as a stable row identifier; s_roundGameNames caches
// every game's player names by that same position so switchToBroadcastGame
// can look up TARGET_WHITE/TARGET_BLACK to match the live stream's
// updates (see handleBroadcastStreamData) back to this particular game.
function fetchRoundGames(roundId) {
  s_roundGameNames = [];

  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;

    var blocks = splitPgnGames(xhr.responseText || '');

    var msgs = [];
    var sentCount = 0;
    for (var i = 0; i < blocks.length; i++) {
      var white = pgnTag(blocks[i], 'White') || 'White';
      var black = pgnTag(blocks[i], 'Black') || 'Black';
      s_roundGameNames[i] = { white: white, black: black };

      var result = pgnTag(blocks[i], 'Result') || '*';
      if (result !== '*' || sentCount >= MAX_BROADCAST_GAMES) continue;
      msgs.push({ 'GameIndex': i, 'GameWhite': white, 'GameBlack': black });
      sentCount++;
    }
    msgs.push({ 'GameListDone': 1 });
    sendQueuedMessages(msgs);
  };
  xhr.onerror = function () {
    console.log('lichess-tv: round game list fetch failed');
    Pebble.sendAppMessage({ 'GameListDone': 1 });
  };
  try {
    xhr.open('GET', 'https://lichess.org/api/broadcast/round/' + roundId + '.pgn', true);
    xhr.send();
  } catch (e) {
    console.log('lichess-tv: round game list open failed: ' + e);
    Pebble.sendAppMessage({ 'GameListDone': 1 });
  }
}

// Switches the live connection over to a broadcast game, mirroring
// setChannel's reset-then-reconnect shape below.
function switchToBroadcastGame(gameIndex) {
  STREAM_MODE = STREAM_MODE_BROADCAST;
  BROADCAST_GAME_INDEX = gameIndex;
  BROADCAST_LABEL = s_broadcastNameById[BROADCAST_ROUND_ID] || 'Broadcast';
  var names = s_roundGameNames[gameIndex];
  TARGET_WHITE = names ? names.white : '';
  TARGET_BLACK = names ? names.black : '';
  computeFeedUrlAndLabel();

  TARGET_STREAMER_NAME = '';
  s_streamerWaitingOffline = false;
  resetGameStreamState();
  s_lastBroadcastBoard = null;
  s_lastPingSentAt = 0;
  s_prevWhiteSecs = null;
  s_prevBlackSecs = null;
  s_activeColor = '';
  s_activeScoreWhite = '';
  s_activeScoreBlack = '';
  sendScoreUpdate('', '');

  try {
    localStorage.setItem(STORAGE_KEY_BROADCAST_ROUND, BROADCAST_ROUND_ID);
    localStorage.setItem(STORAGE_KEY_BROADCAST_GAME, gameIndex);
    localStorage.setItem(STORAGE_KEY_BROADCAST_LABEL, BROADCAST_LABEL);
    // A broadcast game is now "the thing to resume" instead of a
    // streamer - see restoreLastStreamer().
    localStorage.removeItem(STORAGE_KEY_STREAMER);
  } catch (e) {}

  sendChannelSwitching(STATUS_LABEL + ' \u2022 connecting...', true);
  console.log('lichess-tv: switching to broadcast game index ' + gameIndex + ' of round ' + BROADCAST_ROUND_ID);
  connectStream();
}

function setChannel(newChannel) {
  if (!CHANNEL_LABELS.hasOwnProperty(newChannel)) return;
  var wasBroadcast = (STREAM_MODE === STREAM_MODE_BROADCAST);
  if (!wasBroadcast && newChannel === CHANNEL && FEED_URL) return;

  STREAM_MODE = STREAM_MODE_TV;
  CHANNEL = newChannel;
  computeFeedUrlAndLabel();
  s_lastFen = null;
  TARGET_STREAMER_NAME = '';
  s_streamerWaitingOffline = false;
  resetGameStreamState();
  s_lastPingSentAt = 0;
  s_prevWhiteSecs = null;
  s_prevBlackSecs = null;
  s_activeColor = '';
  s_activeScoreWhite = '';
  s_activeScoreBlack = '';
  sendScoreUpdate('', '');
  resetPlayerCache();
  resetGameTracking();

  try {
    localStorage.setItem(STORAGE_KEY_CHANNEL, CHANNEL);
    // A TV channel is now "the thing to resume" instead of a broadcast
    // game - see restoreLastBroadcastGame().
    localStorage.removeItem(STORAGE_KEY_BROADCAST_ROUND);
    localStorage.removeItem(STORAGE_KEY_BROADCAST_GAME);
    localStorage.removeItem(STORAGE_KEY_BROADCAST_LABEL);
    localStorage.removeItem(STORAGE_KEY_STREAMER);
  } catch (e) {}

  sendChannelSwitching(STATUS_LABEL + ' \u2022 connecting...', false);
  console.log('lichess-tv: switching to channel "' + CHANNEL + '"');
  connectStream();
}

// --- Live streamers -------------------------------------------------
//
// https://lichess.org/api/streamer/live lists everyone currently live
// (ND-JSON, one streamer per line). Selecting one looks up their current
// game via /api/user/{username}/current-game; if they're actually mid-
// game right now, we connect the board to that single game's live
// stream (see handleGameStreamData above). If they're live but not
// playing (just chatting, reviewing, etc.), we say so instead of
// showing a stale finished game.
function fetchStreamerList() {
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;

    var items = [];
    if (xhr.status === 200) {
      // Unlike the TV/broadcast/game-stream feeds, this endpoint returns a
      // single plain JSON array ("[{...},{...}]"), not newline-delimited
      // JSON - one JSON.parse of the whole body, not a per-line loop.
      var list = [];
      try {
        list = JSON.parse(xhr.responseText || '[]');
      } catch (e) {
        console.log('lichess-tv: streamer list JSON parse failed: ' + e);
      }
      if (Array.isArray(list)) {
        for (var i = 0; i < list.length && items.length < MAX_STREAMERS; i++) {
          var obj = list[i];
          if (!obj) continue;
          var username = obj.name || obj.id;
          if (!username) continue;
          items.push(username);
        }
      }
    } else {
      console.log('lichess-tv: streamer list fetch returned status ' + xhr.status);
    }

    var msgs = [];
    for (var idx = 0; idx < items.length; idx++) {
      msgs.push({ 'StreamerIndex': idx, 'StreamerName': items[idx] });
    }
    msgs.push({ 'StreamerListDone': 1, 'StreamerCount': items.length });
    sendQueuedMessages(msgs);
  };
  xhr.onerror = function () {
    console.log('lichess-tv: streamer list fetch failed');
    Pebble.sendAppMessage({ 'StreamerListDone': 1, 'StreamerCount': 0 });
  };
  try {
    xhr.open('GET', 'https://lichess.org/api/streamer/live', true);
    xhr.send();
  } catch (e) {
    console.log('lichess-tv: streamer list open failed: ' + e);
    Pebble.sendAppMessage({ 'StreamerListDone': 1, 'StreamerCount': 0 });
  }
}

// Selects `username` as the streamer to watch and looks up whether
// they're currently mid-game. Pass isBackgroundRecheck=true for the
// periodic re-checks scheduleStreamerRecheck() makes while waiting for an
// offline streamer to start playing - that path deliberately leaves
// whatever's already on screen alone (no resetGameStreamState, no
// transient "looking up..."/snapshot repaint) rather than treating every
// poll as a brand new selection.
function switchToStreamerGame(username, isBackgroundRecheck) {
  var alreadyOnThisStreamer = (TARGET_STREAMER_NAME === username && STREAM_MODE === STREAM_MODE_GAME);
  TARGET_STREAMER_NAME = username;
  s_streamerWaitingOffline = true;

  var showedSnapshot = false;
  if (!alreadyOnThisStreamer) {
    resetGameStreamState();

    try {
      localStorage.setItem(STORAGE_KEY_STREAMER, username);
      // A streamer is now "the thing to resume" instead of a broadcast
      // game or TV channel - see restoreLastStreamer().
      localStorage.removeItem(STORAGE_KEY_BROADCAST_ROUND);
      localStorage.removeItem(STORAGE_KEY_BROADCAST_GAME);
      localStorage.removeItem(STORAGE_KEY_BROADCAST_LABEL);
    } catch (e) {}

    // Show whatever we last saw them play, if anything, instead of a
    // blank/placeholder board while we look up whether they're live
    // right now - and so a streamer who turns out to be offline never
    // blanks the board at all, just keeps showing this.
    showedSnapshot = sendCachedStreamerSnapshot(username);
    if (!showedSnapshot) {
      sendChannelSwitching(username + ' \u2022 looking up game...', false, true);
    }
  }

  var lookup = new XMLHttpRequest();
  lookup.onreadystatechange = function () {
    if (lookup.readyState !== 4) return;
    // The user may have navigated away (picked a different streamer, a
    // broadcast, or a TV channel) while this lookup was in flight.
    if (TARGET_STREAMER_NAME !== username) return;

    var gameInfo = null;
    if (lookup.status === 200) {
      try { gameInfo = JSON.parse(lookup.responseText || '{}'); } catch (e) { gameInfo = null; }
    }
    var isOngoing = !!(gameInfo && gameInfo.id &&
      (gameInfo.status === 'started' || gameInfo.status === 'created'));

    if (!isOngoing) {
      // Not currently playing - leave whatever's already on screen (the
      // cached snapshot, or the live board they were mid-game on right
      // up until now) exactly as is rather than switching away, and keep
      // checking back for their next game.
      if (!alreadyOnThisStreamer && !showedSnapshot) {
        sendStatusOnly(username + ' isn\u2019t playing right now');
      }
      scheduleStreamerRecheck();
      return;
    }

    STREAM_MODE = STREAM_MODE_GAME;
    STATUS_LABEL = username;
    FEED_URL = 'https://lichess.org/api/stream/game/' + gameInfo.id;
    s_streamerWaitingOffline = false;

    resetGameStreamState();
    s_lastPingSentAt = 0;
    s_prevWhiteSecs = null;
    s_prevBlackSecs = null;
    s_activeColor = '';
    s_activeScoreWhite = '';
    s_activeScoreBlack = '';
    sendScoreUpdate('', '');

    console.log('lichess-tv: switching to streamer "' + username + '" game ' + gameInfo.id);
    connectStream();
  };
  lookup.onerror = function () {
    if (TARGET_STREAMER_NAME === username) scheduleStreamerRecheck();
  };
  try {
    lookup.open('GET', 'https://lichess.org/api/user/' + encodeURIComponent(username) + '/current-game?evals=false', true);
    lookup.setRequestHeader('Accept', 'application/json');
    lookup.send();
  } catch (e) {
    scheduleStreamerRecheck();
  }
}

Pebble.addEventListener('appmessage', function (e) {
  var payload = e.payload || {};
  if (typeof payload.SelectedChannel !== 'undefined') {
    setChannel(payload.SelectedChannel);
  }
  if (typeof payload.CheckStream !== 'undefined') {
    sendStreamAliveReply();
  }
  if (typeof payload.EnterBroadcastMode !== 'undefined') {
    fetchBroadcastList();
  }
  if (typeof payload.EnterStreamerMode !== 'undefined') {
    fetchStreamerList();
  }
  if (typeof payload.SelectedStreamer !== 'undefined') {
    switchToStreamerGame(payload.SelectedStreamer);
  }
  if (typeof payload.SelectedBroadcast !== 'undefined') {
    BROADCAST_ROUND_ID = payload.SelectedBroadcast;
    fetchRoundGames(BROADCAST_ROUND_ID);
  }
  if (typeof payload.SelectedGame !== 'undefined') {
    switchToBroadcastGame(parseInt(payload.SelectedGame, 10));
  }
  if (typeof payload.ShowTournamentPlayers !== 'undefined') {
    // Re-fetch rather than reuse s_roundGameNames: that array only holds
    // player names, not "still live" status, and games can have finished
    // since it was last populated.
    fetchRoundGames(BROADCAST_ROUND_ID);
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

// Attempts to resume the last-watched streamer that switchToStreamerGame
// persisted (see STORAGE_KEY_STREAMER above). Unlike a TV channel or
// broadcast game, there's no separate fetch needed to validate this -
// switchToStreamerGame does its own lookup and, since it immediately
// shows any cached snapshot (see sendCachedStreamerSnapshot) and never
// falls back to something else on failure, calling it here is always a
// "restore", never a dead end. Calls back with true if there was a
// streamer to resume at all, false otherwise so the caller can fall back
// to the normal broadcast/TV channel flow.
function restoreLastStreamer(callback) {
  var username;
  try { username = localStorage.getItem(STORAGE_KEY_STREAMER); } catch (e) {}
  if (!username) { callback(false); return; }

  console.log('lichess-tv: resuming streamer "' + username + '"');
  switchToStreamerGame(username);
  callback(true);
}

// Attempts to resume the last-watched broadcast (tournament) game that
// switchToBroadcastGame persisted (see STORAGE_KEY_BROADCAST_ROUND/GAME
// above). Re-fetches the round's PGN first - needed to rebuild
// s_roundGameNames for TARGET_WHITE/TARGET_BLACK matching, and doubles as
// a check that the saved game index is still valid (a round can in theory
// end up with fewer games, though not fewer players, between sessions).
// Calls back with true if the resume actually happened (switchToBroadcastGame
// was called), false otherwise so the caller can fall back to the normal
// TV channel flow.
function restoreLastBroadcastGame(callback) {
  var roundId, gameIndex;
  try {
    roundId = localStorage.getItem(STORAGE_KEY_BROADCAST_ROUND);
    gameIndex = parseInt(localStorage.getItem(STORAGE_KEY_BROADCAST_GAME), 10);
  } catch (e) {
    callback(false);
    return;
  }
  if (!roundId || isNaN(gameIndex) || gameIndex < 0) {
    callback(false);
    return;
  }

  var label = 'Broadcast';
  try {
    label = localStorage.getItem(STORAGE_KEY_BROADCAST_LABEL) || label;
  } catch (e) {}

  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;
    if (xhr.status !== 200) { callback(false); return; }

    var blocks = splitPgnGames(xhr.responseText || '');
    if (gameIndex >= blocks.length) {
      console.log('lichess-tv: saved broadcast game index no longer valid, falling back to TV');
      callback(false);
      return;
    }

    s_roundGameNames = [];
    for (var i = 0; i < blocks.length; i++) {
      s_roundGameNames[i] = {
        white: pgnTag(blocks[i], 'White') || 'White',
        black: pgnTag(blocks[i], 'Black') || 'Black'
      };
    }

    BROADCAST_ROUND_ID = roundId;
    s_broadcastNameById[roundId] = label;
    console.log('lichess-tv: resuming broadcast game index ' + gameIndex + ' of round ' + roundId);
    switchToBroadcastGame(gameIndex);
    callback(true);
  };
  xhr.onerror = function () { callback(false); };
  try {
    xhr.open('GET', 'https://lichess.org/api/broadcast/round/' + roundId + '.pgn', true);
    xhr.send();
  } catch (e) {
    callback(false);
  }
}

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

  restoreLastStreamer(function (streamerRestored) {
    if (streamerRestored) return;
    restoreLastBroadcastGame(function (broadcastRestored) {
      if (!broadcastRestored) connectStream();
    });
  });
});