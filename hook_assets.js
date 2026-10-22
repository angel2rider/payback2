// Payback 2 — asset manifest + live \0IAP capture
'use strict';

var opened = Object.create(null);
var order = [];
var handlePath = Object.create(null);   // AAsset* -> path
var readBuf = Object.create(null);      // path -> {ptr, total}
var CAPTURE_MAX = 4194304;              // 4 MB cap per asset

function log(x) { send({ t: 'log', m: String(x) }); }

function noteOp(path, n) {
    if (!opened[path]) { opened[path] = { reads: 0, bytes: 0 }; order.push(path); }
    opened[path].reads++;
    opened[path].bytes += n;
}

function attach() {
    var la = Process.findModuleByName('libandroid.so');
    if (!la) { setTimeout(attach, 50); return; }
    log('libandroid @ ' + la.base);

    function hk(name, cb) {
        var a = la.findExportByName(name);
        if (!a) { log('MISSING ' + name); return; }
        Interceptor.attach(a, cb);
    }

    function regOpen(args) {
        var p = args[1];
        return p.isNull() ? null : p.readCString();
    }

    hk('AAssetManager_open', {
        onEnter: function (a) { this.p = regOpen(a); },
        onLeave: function (r) {
            if (this.p === null) return;
            noteOp(this.p, 0);
            if (!r.isNull()) handlePath[r.toString()] = this.p;
        }
    });

    hk('AAsset_open', {
        onEnter: function (a) { this.p = regOpen(a); },
        onLeave: function (r) {
            if (this.p === null) return;
            noteOp(this.p, 0);
            if (!r.isNull()) handlePath[r.toString()] = this.p;
        }
    });

    hk('AAsset_getLength', {
        onEnter: function (a) { this.h = a[0].toString(); },
        onLeave: function (r) {
            var p = handlePath[this.h];
            if (p !== undefined) noteOp(p, 0);
        }
    });

    hk('AAsset_read', {
        onEnter: function (a) {
            this.h = a[0].toString();
            this.buf = a[1];
            this.n = a[2].toUInt32();
        },
        onLeave: function (r) {
            var p = handlePath[this.h];
            if (p === undefined) return;
            var got = r.toInt32();
            if (got <= 0) return;
            noteOp(p, got);
            if (p.indexOf('\u0000IAP') !== 0) return;
            if (readBuf[p] === undefined) readBuf[p] = { ptr: this.buf, total: 0 };
            readBuf[p].ptr = this.buf;
            readBuf[p].total += got;
        }
    });

    hk('AAsset_close', {
        onEnter: function (a) {
            var h = a[0].toString();
            var p = handlePath[h];
            delete handlePath[h];
            if (p === undefined) return;
            var rb = readBuf[p];
            if (rb === undefined || rb.total === 0) return;
            delete readBuf[p];
            var n = Math.min(rb.total, CAPTURE_MAX);
            var data;
            try { data = ptr(rb.ptr).readByteArray(n); }
            catch (e) { log('read fail ' + p + ': ' + e); return; }
            log('IAPCAP ' + p + ' ' + n);
            send({ t: 'iap', name: p, size: n }, data);
        }
    });

    rpc.exports = {
        manifest: function () {
            var rows = [];
            for (var i = 0; i < order.length; i++) {
                var p = order[i];
                rows.push([p, opened[p].reads, opened[p].bytes]);
            }
            return rows;
        }
    };
}

attach();