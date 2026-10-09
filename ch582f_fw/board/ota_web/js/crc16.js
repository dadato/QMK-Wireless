/* CRC-16/CCITT-FALSE —— 与 core/bridge_crc16.c、tools/sk32sim/proto.py 逐位一致
 *
 * 参数：init = 0xFFFF，poly = 0x1021，不反转，xorout = 0
 * 自检：crc16("123456789") === 0x29B1
 */
(function (global) {
  'use strict';

  var TABLE = (function () {
    var t = new Uint16Array(256);
    for (var i = 0; i < 256; i++) {
      var c = (i << 8) & 0xFFFF;
      for (var b = 0; b < 8; b++) {
        c = (c & 0x8000) ? (((c << 1) ^ 0x1021) & 0xFFFF) : ((c << 1) & 0xFFFF);
      }
      t[i] = c;
    }
    return t;
  })();

  /** @param {Uint8Array|Array<number>} data @param {number} [crc] @returns {number} */
  function crc16(data, crc) {
    var c = (crc === undefined || crc === null) ? 0xFFFF : (crc & 0xFFFF);
    for (var i = 0; i < data.length; i++) {
      c = (((c << 8) & 0xFFFF) ^ TABLE[((c >> 8) ^ (data[i] & 0xFF)) & 0xFF]) & 0xFFFF;
    }
    return c;
  }

  var api = { crc16: crc16, TABLE: TABLE };
  global.OtaCrc = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);