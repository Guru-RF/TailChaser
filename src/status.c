/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "status.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct status {
    pthread_mutex_t   mu;
    status_snapshot_t snap;
    int               lfd;
    pthread_t         thread;
    bool              running;
    atomic_bool       stop;
};

static const char PAGE[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>TailChaser</title><style>"
":root{--bg:#0b0d10;--card:#12151a;--fg:#f5f3ee;--mut:#9aa0ab;--line:#262b34;"
"--ok:#6ad19b;--okbg:#14301f;--bad:#ff8a80;--badbg:#3b1c1a;--warn:#f9b934;--warnbg:#3a2c0e;--acc:#f9b934}"
"@media(prefers-color-scheme:light){:root{--bg:#f6f4ef;--card:#fff;--fg:#14171c;--mut:#667085;--line:#e4e0d6;"
"--ok:#16794c;--okbg:#e2f5ea;--bad:#b3261e;--badbg:#fbe9e7;--warn:#8a5a00;--warnbg:#fdf0d0;--acc:#c98a06}}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);"
"font:15px/1.45 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}"
"main{max-width:980px;margin:0 auto;padding:20px 16px 40px}"
"header{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin:0 0 6px}"
".logo{width:52px;height:52px;flex:none;border-radius:13px;background:var(--card);border:1.5px solid #6b5327}"
".logo path{fill:none;stroke:#f9b934;stroke-width:6.5;stroke-linecap:round;stroke-linejoin:round}"
"h1{font-size:30px;line-height:1;margin:0;font-weight:800;letter-spacing:-.02em}h1 b{color:var(--acc);font-weight:800}"
"h1 i{font-style:normal;color:var(--mut);font-weight:600}"
".tag{font:11px ui-monospace,Menlo,Consolas,monospace;letter-spacing:.28em;color:var(--mut);margin-top:6px}"
".call{margin-left:auto;font-size:26px;font-weight:700;font-variant-numeric:tabular-nums}"
".sub{color:var(--mut);margin:0 0 18px;font-size:13px}"
"footer{color:var(--mut);font-size:12px;margin-top:18px}footer a{color:var(--acc);text-decoration:none}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;margin-bottom:12px}"
".card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}"
".k{font-size:12px;color:var(--mut);text-transform:uppercase;letter-spacing:.04em}"
".v{font-size:22px;font-weight:600;margin-top:2px;font-variant-numeric:tabular-nums}"
".s{font-size:13px;color:var(--mut);margin-top:2px}"
".pill{display:inline-block;padding:1px 9px;border-radius:999px;font-size:12px;font-weight:600}"
".ok{background:var(--okbg);color:var(--ok)}.bad{background:var(--badbg);color:var(--bad)}"
".warn{background:var(--warnbg);color:var(--warn)}.idle{background:var(--line);color:var(--mut)}"
".bar{height:8px;background:var(--line);border-radius:4px;overflow:hidden;margin-top:8px}"
".bar i{display:block;height:100%;background:var(--acc);width:0;transition:width .2s}"
".tw{overflow-x:auto}table{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}"
"th{text-align:left;color:var(--mut);font-weight:500;padding:6px 10px 6px 0;border-bottom:1px solid var(--line);white-space:nowrap}"
"td{padding:6px 10px 6px 0;border-bottom:1px solid var(--line);white-space:nowrap}"
"h2{font-size:14px;margin:0 0 8px}.r{text-align:right}.gap{margin-bottom:12px}"
".smr{display:flex;align-items:flex-start;gap:14px;margin-top:8px}"
".smc{flex:1;min-width:0}.smb{display:flex;gap:3px;height:40px}.smb b{flex:1;border-radius:3px;background:var(--line)}"
".smb b.g.on{background:#2fbf71}.smb b.y.on{background:#f0b429}.smb b.rd.on{background:#e5484d}"
".smv{font-size:28px;font-weight:700;line-height:40px;width:110px;text-align:right;font-variant-numeric:tabular-nums}"
".smv.tx{color:var(--bad)}"
".smt{display:flex;gap:3px;margin-top:4px;font-size:11px;color:var(--mut)}"
".smt span{flex:1 1 0;min-width:0;display:flex;justify-content:center;white-space:nowrap}"
"@media(max-width:560px){.smv{width:74px;font-size:21px}.smt span:nth-child(even){visibility:hidden}}"
"</style></head><body><main>"
"<header><svg class=logo viewBox='0 0 100 100' aria-hidden=true>"
"<path d='M25 38a26 26 0 0 1 44-11'/><path d='M72 20v11H61'/>"
"<path d='M75 62a26 26 0 0 1-44 11'/><path d='M28 80V69h11'/>"
"<path d='M29 50h11l5-7 6 14 5-10 3 3h12' style='stroke-width:5'/></svg>"
"<div><h1>Tail<b>Chaser</b><i id=role></i></h1><div class=tag>REPEATER SOFTWARE</div></div>"
"<div class=call id=h1></div></header><p class=sub id=sub>&nbsp;</p>"
"<div class='card gap'><div class=k>Signal</div><div class=smr><div class=smc><div class=smb id=smb></div><div class=smt id=smt></div></div>"
"<div class=smv id=smv></div></div><div class=s id=sms>&nbsp;</div></div>"
"<div class=grid>"
"<div class=card id=clink><div class=k>Reflector</div><div class=v id=link>&hellip;</div><div class=s id=links></div></div>"
"<div class=card id=cup><div class=k>Repeater</div><div class=v id=up>&hellip;</div><div class=s id=ups></div></div>"
"<div class=card><div class=k>Receiver</div><div class=v id=rx>&hellip;</div><div class=s id=rxs></div>"
"<div class=bar><i id=lvl></i></div></div>"
"<div class=card id=ctx><div class=k>Transmitter</div><div class=v id=tx>&hellip;</div><div class=s id=txs></div></div>"
"<div class=card id=cid><div class=k>Identification</div><div class=v id=id>&hellip;</div><div class=s id=ids></div></div>"
"</div><div class=grid>"
"<div class=card><div class=k id=kpass>Radio &rarr; reflector</div><div class=v id=pass>0</div><div class=s id=passs></div></div>"
"<div class=card id=cblk><div class=k id=kblk>Kept off the reflector</div><div class=v id=blk>0</div><div class=s id=blks></div></div>"
"<div class=card id=cnet><div class=k>Reflector &rarr; radio</div><div class=v id=net>0</div><div class=s id=nets></div></div>"
"</div>"
"<div class='card gap'><h2>Heard on the radio</h2><div class=tw><table><thead><tr>"
"<th>Time</th><th class=r>Length</th><th>Outcome</th><th>Sounded like</th>"
"<th class=r>Voice after</th><th class=r>Voiced</th><th class=r>Level</th><th class=r>RSSI</th>"
"</tr></thead><tbody id=rows></tbody></table></div></div>"
"<div class=card id=cnh><h2>Heard from the reflector</h2><div class=tw><table><thead><tr>"
"<th>Time</th><th class=r>Length</th><th>Station</th><th class=r>Talkgroup</th><th>On the air</th>"
"</tr></thead><tbody id=nrows></tbody></table></div></div>"
"<footer><span id=ver>TailChaser</span> · <a href='https://tailchaser.app'>tailchaser.app</a>"
" · free software, GPL-3.0-or-later</footer>"
"<script>"
"const $=i=>document.getElementById(i),E=s=>String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));"
"function pill(c,t){return '<span class=\"pill '+c+'\">'+E(t)+'</span>'}"
"function hm(t){return t?new Date(t*1000).toLocaleTimeString():'–'}"
"function dur(s){return s<90?s.toFixed(0)+' s':s<5400?(s/60).toFixed(0)+' min':(s/3600).toFixed(1)+' h'}"
"const SL=['1','2','3','4','5','6','7','8','9','+10','+20','+30','+40','+50','+60'];"
"$('smb').innerHTML=SL.map((x,i)=>'<b class='+(i<4?'g':i<9?'y':'rd')+'></b>').join('');"
"$('smt').innerHTML=SL.map(x=>'<span>'+x+'</span>').join('');"
"let pk=-1,pkt=0;"
"function sl(g,r){return Math.max(1,Math.min(15,Math.round(1+(r-g.rssi_lo)/(g.rssi_hi-g.rssi_lo)*12)))}"
"function sn(l){return l<=9?'S'+l:'S9+'+(l-9)*10}"
"function meter(d,g,x){const deaf=x.keyed&&!d.has.repeater;"
"if(d.has.rxlink){const on=d.receiver.squelch,l=on?Math.max(1,Math.min(15,Math.round(d.receiver.siglev*15/100))):0;"
"[...$('smb').children].forEach((b,i)=>b.classList.toggle('on',i<l));$('smv').textContent=on?Math.round(d.receiver.siglev):'';$('smv').className='smv';"
"$('sms').textContent=(on?'signal '+d.receiver.siglev.toFixed(0)+' of 100, as the receiver reports it':'no station on the input')+' · receiver '+d.receiver.peer;return}"
"const r=g.cos&&!deaf?g.rssi:-1,l=r>=0?sl(g,r):0;"
"[...$('smb').children].forEach((b,i)=>b.classList.toggle('on',i<l));"
"const v=$('smv');v.textContent=deaf?'TX':r>=0?sn(l):'';v.className=deaf?'smv tx':'smv';"
"if(r>=0){if(r>=pk||Date.now()-pkt>60000)pk=r;pkt=Date.now()}"
"$('sms').textContent=(deaf?'transmitting — the receiver is deaf':r>=0?'RSSI '+r:g.cos?'carrier — no reading yet':'squelch closed')+"
"(pk>=0?' · last peak '+sn(sl(g,pk))+' (RSSI '+pk+')':'')+' · scale: RSSI '+g.rssi_lo+' = S1, '+g.rssi_hi+' = S9+40'}"
"function show(i,on){$(i).style.display=on?'':'none'}"
"async function tick(){let d;try{d=await (await fetch('status.json',{cache:'no-store'})).json()}"
"catch(e){$('link').innerHTML=pill('bad','node unreachable');return}"
"const r=d.radio,l=d.link,g=d.gate,x=d.tx,t=d.totals,i=d.ident,h=d.has,u=d.uplink,rv=d.receiver;"
"const dest=h.uplink?'repeater':'reflector';"
"show('clink',h.reflector);show('cup',h.uplink);show('ctx',h.tx);show('cid',h.tx);show('cnet',h.tx);show('cnh',h.tx);show('cblk',h.rx);"
"$('kpass').innerHTML=h.rxlink?'Receiver &rarr; reflector':'Radio &rarr; '+dest;$('kblk').textContent='Kept off the '+dest;"
"const Role=d.role[0].toUpperCase()+d.role.slice(1);$('role').textContent='-'+Role;$('h1').textContent=i.callsign||l.callsign||'';"
"document.title=((i.callsign||l.callsign)?(i.callsign||l.callsign)+' · ':'')+'TailChaser-'+Role;$('ver').textContent='TailChaser-'+Role+' '+d.version+' · '+d.title;"
"$('sub').textContent=(r.type=='none'?'radio on the sound card':"
"(h.rx&&h.tx?r.freq_mhz.toFixed(4)+' MHz'+(r.freq_tx_mhz!=r.freq_mhz?' (TX '+r.freq_tx_mhz.toFixed(4)+')':''):h.tx?'TX '+r.freq_tx_mhz.toFixed(4)+' MHz':'RX '+r.freq_mhz.toFixed(4)+' MHz')+' · '+r.bandwidth+"
"(h.rx?' · CTCSS RX '+r.ctcss_rx:'')+(h.tx?' · CTCSS TX '+r.ctcss_tx:'')+(h.rx?' · squelch '+r.squelch:''))+(h.rx?' · gate '+g.mode:'')+"
"(d.norm.enabled&&(h.rx||h.rxlink)?' · levelled':'');"
"if(h.uplink){$('up').innerHTML=u.connected?pill('ok','connected'):pill('bad','not connected');"
"$('ups').textContent=u.connected?(u.peer+' · '+(u.codec||'no codec yet')+(u.mute==2?' · muted':'')):'waiting on TCP port '+u.listen_port+' · RemoteTrx 2.'+u.proto_minor}"
"$('link').innerHTML=l.connected?pill('ok','TG '+l.tg):l.waiting_for_cert?pill('warn','waiting for certificate'):pill('bad','not connected');"
"$('links').textContent=l.connected?(l.callsign+' on '+l.host+(l.talker?' · '+l.talker+' is talking':'')+(l.tg!=l.home_tg?' · home TG '+l.home_tg:'')):"
"l.waiting_for_cert?'the reflector sysop has to sign the request of '+l.callsign:(l.host?'retrying '+l.host:'looking for the reflector');"
"meter(d,g,x);if(h.rxlink){$('rx').innerHTML=!rv.connected?pill('bad','receiver not connected'):rv.squelch?pill('ok','station on the input'):pill('idle','quiet');"
"$('rxs').textContent=rv.peer+(rv.connected?' · '+rv.codec+' · RemoteTrx '+rv.proto:'')}else{"
"$('rx').innerHTML=x.keyed&&!h.repeater?pill('idle','deaf — transmitting'):g.open?pill('ok','voice — to the '+dest):g.cos?pill('warn','carrier — listening for a voice'):pill('idle','quiet');"
"$('rxs').textContent=g.cos?('level '+g.level_db.toFixed(0)+' dBFS'+(g.rssi>=0?' · RSSI '+g.rssi:'')+(g.voiced?' · voiced':'')+"
"(d.ctcss.detect?' · tone '+d.ctcss.level_db.toFixed(0)+' dBFS, '+d.ctcss.snr_db.toFixed(0)+' dB clear':'')):"
"(g.hold_left_ms>0?'in conversation for another '+Math.ceil(g.hold_left_ms/1000)+' s':'squelch closed')}"
"$('lvl').style.width=(g.cos&&h.rx?Math.max(0,Math.min(100,(g.level_db+70)*100/70)):0)+'%';"
"$('tx').innerHTML=x.keyed?pill('bad','on the air — '+x.why):pill('idle','off');"
"$('txs').textContent=(x.keyed?(x.keyed_ms/1000).toFixed(0)+' s'+(x.timeout_s?' of '+x.timeout_s+' s':'')+' · ':'')+x.count+(x.count==1?' transmission · ':' transmissions · ')+dur(x.total_s)+"
"(x.timeouts?' · '+x.timeouts+' timed out':'');"
"$('id').textContent=i.interval_min?i.callsign:'off';"
"$('ids').textContent=i.interval_min?('CW · next '+hm(i.next)+' · last '+hm(i.last)):'no identification';"
"$('pass').textContent=t.passed;$('passs').textContent=(h.rxlink?'overs from the receiver':'of '+t.openings+(t.openings==1?' squelch opening':' squelch openings'))+(t.dtmf_commands?' · '+t.dtmf_commands+' DTMF commands':'');"
"$('blk').textContent=g.mode=='monitor'?t.would_block:t.blocked;"
"$('blks').textContent=g.mode=='monitor'?'would have been blocked (monitor mode: nothing is)':"
"g.mode=='off'?'gate is off':t.blocked_noise+' noise · '+t.blocked_silence+' dead carrier · '+t.blocked_tone+' tone';"
"$('net').textContent=t.net_overs;$('nets').textContent='overs received'+(t.net_blocked?' · '+t.net_blocked+' not sent: channel in use':'');"
"$('rows').innerHTML=d.history.map(o=>{const blocked=o.pass=='blocked',wb=o.pass=='ungated'&&o.analysed&&o.would=='blocked';"
"return '<tr><td>'+hm(o.t)+'</td><td class=r>'+(o.ms/1000).toFixed(2)+' s</td><td>'+"
"(blocked?pill('bad','blocked'):!o.sent?pill('warn','not sent'):wb?pill('warn',(o.tg?'TG '+o.tg:'passed')+' · would block'):pill('ok',(o.tg?'TG '+o.tg:'passed')+(o.would=='ungated'||o.would=='receiver'?'':' · '+o.would)))+'</td><td>'+"
"(o.analysed?E(o.like):'–')+(o.dtmf?' + DTMF':'')+'</td><td class=r>'+(o.speech_at_ms>=0?o.speech_at_ms+' ms':'–')+'</td><td class=r>'+"
"(o.analysed?o.voiced_ms+' ms':'–')+'</td><td class=r>'+(o.analysed?o.level_db.toFixed(0)+' dBFS':'–')+'</td><td class=r>'+(o.rssi>=0?o.rssi:'–')+'</td></tr>'}).join('')"
"||'<tr><td colspan=8 style=\"color:var(--mut)\">Nothing received yet.</td></tr>';"
"$('nrows').innerHTML=d.net_history.map(o=>'<tr><td>'+hm(o.t)+'</td><td class=r>'+(o.ms/1000).toFixed(1)+' s</td><td>'+E(o.call||'?')+'</td><td class=r>'+o.tg+'</td><td>'+"
"pill(o.outcome=='transmitted'?'ok':o.outcome=='partly'?'warn':'bad',o.outcome)+'</td></tr>').join('')"
"||'<tr><td colspan=5 style=\"color:var(--mut)\">Nothing received yet.</td></tr>'}"
"tick();setInterval(tick,300)"
"</script></main></body></html>";

typedef struct { char *p; size_t len, cap; } sbuf_t;

static void sb_printf(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sbuf_t *b, const char *fmt, ...)
{
    if (b->len >= b->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        b->len += (size_t)n < b->cap - b->len ? (size_t)n : b->cap - b->len - 1;
}

/* JSON string body: our strings are ASCII identifiers, but be safe. */
static const char *js(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 2 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch >= 0x20)         { out[o++] = (char)ch; }
    }
    out[o] = '\0';
    return out;
}

#define B(x) ((x) ? "true" : "false")

static size_t build_json(status_t *s, char *out, size_t cap)
{
    sbuf_t b = { out, 0, cap };
    char t1[600], t2[128], t3[128], t4[128];
    pthread_mutex_lock(&s->mu);
    const status_snapshot_t *d = &s->snap;
    time_t now = util_wall_time();
    sb_printf(&b, "{\"version\":\"%s\",\"board\":\"%s\",\"uptime_s\":%ld,\"now\":%ld,",
              js(d->version, t1, sizeof t1), js(d->board, t2, sizeof t2),
              (long)(now - d->started), (long)now);
    sb_printf(&b, "\"role\":\"%s\",\"title\":\"%s\",\"has\":{\"rx\":%s,\"tx\":%s,\"reflector\":%s,"
                  "\"uplink\":%s,\"rxlink\":%s,\"repeater\":%s},",
              js(d->role, t1, sizeof t1), js(d->title, t2, sizeof t2), B(d->has_rx), B(d->has_tx),
              B(d->has_reflector), B(d->has_uplink), B(d->has_rxlink), B(d->repeater));
    sb_printf(&b, "\"uplink\":{\"connected\":%s,\"peer\":\"%s\",\"codec\":\"%s\",\"mute\":%d,"
                  "\"connects\":%lu,\"auth_failures\":%lu,\"frames\":%lu,\"proto_minor\":%d,"
                  "\"listen_port\":%d},",
              B(d->uplink.connected), js(d->uplink.peer, t1, sizeof t1),
              js(d->uplink.codec, t2, sizeof t2), d->uplink.mute_state, d->uplink.connects,
              d->uplink.auth_failures, d->uplink.audio_frames_sent, d->uplink.proto_minor,
              d->listen_port);
    sb_printf(&b, "\"receiver\":{\"connected\":%s,\"peer\":\"%s\",\"codec\":\"%s\",\"proto\":\"%d.%d\","
                  "\"squelch\":%s,\"siglev\":%.1f,\"connects\":%lu,\"auth_failures\":%lu,\"frames\":%lu},",
              B(d->rxlink.connected), js(d->rxlink.peer, t1, sizeof t1),
              js(d->rxlink.codec, t2, sizeof t2), d->rxlink.proto_major, d->rxlink.proto_minor,
              B(d->rxlink.squelch_open), (double)d->rxlink.siglev, d->rxlink.connects,
              d->rxlink.auth_failures, d->rxlink.audio_frames);
    sb_printf(&b, "\"norm\":{\"enabled\":%s,\"gain_db\":%.1f},", B(d->normalize), (double)d->norm_gain_db);
    sb_printf(&b, "\"noise\":{\"meter\":%s,\"level_db\":%.1f},", B(d->noise_meter), (double)d->noise_db);
    sb_printf(&b, "\"ctcss\":{\"detect\":%s,\"tone_hz\":%.1f,\"open\":%s,\"level_db\":%.1f,\"snr_db\":%.1f},",
              B(d->tone_detect), (double)d->tone_hz, B(d->tone_detect && d->tone_open),
              (double)(d->tone_detect ? d->tone_level_db : -120.0f), (double)(d->tone_detect ? d->tone_snr_db : 0.0f));
    sb_printf(&b, "\"radio\":{\"freq_mhz\":%.4f,\"freq_tx_mhz\":%.4f,\"bandwidth\":\"%s\","
                  "\"ctcss_rx\":\"%s\",\"ctcss_tx\":\"%s\",\"squelch\":%d,\"firmware\":\"%s\",\"type\":\"%s\"},",
              d->freq_rx_mhz, d->freq_tx_mhz, js(d->bandwidth, t1, sizeof t1),
              js(d->ctcss_rx, t2, sizeof t2), js(d->ctcss_tx, t3, sizeof t3), d->squelch,
              js(d->firmware, t4, sizeof t4), d->radio_type);
    sb_printf(&b, "\"link\":{\"connected\":%s,\"audio\":%s,\"waiting_for_cert\":%s,\"host\":\"%s\","
                  "\"port\":%d,\"proto\":%d,\"callsign\":\"%s\",\"tg\":%u,\"home_tg\":%u,"
                  "\"talker\":\"%s\",\"connects\":%lu,\"rx_frames\":%lu,\"tx_frames\":%lu},",
              B(d->link.connected), B(d->link.udp_ready), B(d->link.waiting_for_cert),
              js(d->link.host, t1, sizeof t1), d->link.port, d->link.proto_major,
              js(d->callsign, t2, sizeof t2), (unsigned)d->tg, (unsigned)d->home_tg,
              js(d->talker, t3, sizeof t3), d->link.connects, d->link.rx_frames,
              d->link.tx_frames);
    sb_printf(&b, "\"tx\":{\"keyed\":%s,\"why\":\"%s\",\"keyed_ms\":%d,\"timeout_s\":%d,"
                  "\"count\":%lu,\"total_s\":%lu,\"timeouts\":%lu},",
              B(d->tx), js(d->tx_why, t1, sizeof t1), d->tx_ms, d->tx_timeout_s,
              d->tx_count, d->tx_total_ms / 1000, d->tx_timeouts);
    sb_printf(&b, "\"ident\":{\"callsign\":\"%s\",\"interval_min\":%d,\"last\":%ld,\"next\":%ld,"
                  "\"count\":%lu},",
              js(d->ident_callsign, t1, sizeof t1), d->ident_interval_min,
              (long)d->ident_last, (long)d->ident_next, d->idents);
    sb_printf(&b, "\"gate\":{\"mode\":\"%s\",\"hold_s\":%d,\"cos\":%s,\"open\":%s,\"state\":\"%s\","
                  "\"hold_left_ms\":%d,\"level_db\":%.1f,\"periodicity\":%.2f,\"voiced\":%s,"
                  "\"rssi\":%d,\"rssi_lo\":%d,\"rssi_hi\":%d,\"siglev\":%.1f,\"overruns\":%lu,\"underruns\":%lu},",
              js(d->mode, t1, sizeof t1), d->hold_s, B(d->cos_open), B(d->gate_open),
              js(d->gate_state, t2, sizeof t2), d->hold_left_ms, d->level_db, d->periodicity,
              B(d->voiced), d->rssi, d->rssi_lo, d->rssi_hi, (double)d->siglev, d->overruns,
              d->underruns);
    sb_printf(&b, "\"totals\":{\"openings\":%lu,\"passed\":%lu,\"blocked\":%lu,\"blocked_noise\":%lu,"
                  "\"blocked_silence\":%lu,\"blocked_tone\":%lu,\"would_block\":%lu,"
                  "\"dtmf_digits\":%lu,\"dtmf_commands\":%lu,\"net_overs\":%lu,\"net_blocked\":%lu},",
              d->openings, d->passed, d->blocked, d->blocked_noise, d->blocked_silence,
              d->blocked_tone, d->would_block, d->dtmf_digits, d->dtmf_commands,
              d->net_overs, d->net_blocked);
    sb_printf(&b, "\"history\":[");
    for (int i = 0; i < d->hist_len; i++) {     /* newest first */
        const status_opening_t *o =
            &d->history[(d->hist_head - 1 - i + 2 * STATUS_HISTORY) % STATUS_HISTORY];
        sb_printf(&b, "%s{\"t\":%ld,\"ms\":%d,\"pass\":\"%s\",\"would\":\"%s\",\"like\":\"%s\",\"analysed\":%s,"
                      "\"speech_at_ms\":%d,\"voiced_ms\":%d,\"delay_ms\":%d,\"level_db\":%.1f,"
                      "\"rssi\":%d,\"dtmf\":%d,\"tg\":%u,\"sent\":%s}",
                  i ? "," : "", (long)o->when, o->duration_ms, js(o->pass, t1, sizeof t1),
                  js(o->would, t3, sizeof t3), js(o->looks_like, t2, sizeof t2), B(o->analysed),
                  o->speech_at_ms, o->voiced_ms, o->delay_ms, o->level_avg_db, o->rssi,
                  o->dtmf_digits, (unsigned)o->tg, B(o->sent));
    }
    sb_printf(&b, "],\"net_history\":[");
    for (int i = 0; i < d->net_len; i++) {
        const status_netover_t *o =
            &d->net_history[(d->net_head - 1 - i + 2 * STATUS_NET_HISTORY) % STATUS_NET_HISTORY];
        sb_printf(&b, "%s{\"t\":%ld,\"ms\":%d,\"call\":\"%s\",\"tg\":%u,\"outcome\":\"%s\"}",
                  i ? "," : "", (long)o->when, o->duration_ms, js(o->call, t1, sizeof t1),
                  (unsigned)o->tg, js(o->outcome, t2, sizeof t2));
    }
    sb_printf(&b, "]}\n");
    pthread_mutex_unlock(&s->mu);
    return b.len;
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void respond(int fd, const char *status, const char *type, const char *body, size_t len)
{
    char h[256];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, len);
    send_all(fd, h, (size_t)n);
    send_all(fd, body, len);
}

static void serve(status_t *s, int fd)
{
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[1024];
    size_t got = 0;
    while (got < sizeof req - 1) {
        ssize_t r = recv(fd, req + got, sizeof req - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[got] = '\0';
    char method[8] = "", path[128] = "";
    sscanf(req, "%7s %127s", method, path);
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (strcmp(method, "GET") != 0) {
        respond(fd, "405 Method Not Allowed", "text/plain", "read-only\n", 10);
    } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(fd, "200 OK", "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
    } else if (!strcmp(path, "/status.json")) {
        static _Thread_local char json[40960];
        size_t n = build_json(s, json, sizeof json);
        respond(fd, "200 OK", "application/json", json, n);
    } else {
        respond(fd, "404 Not Found", "text/plain", "not found\n", 10);
    }
}

static void *http_main(void *arg)
{
    status_t *s = arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd p = { s->lfd, POLLIN, 0 };
        if (poll(&p, 1, 250) <= 0)
            continue;
        int fd = accept4(s->lfd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        serve(s, fd);
        close(fd);
    }
    return NULL;
}

status_t *status_new(const char *bind_addr, int port)
{
    status_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    pthread_mutex_init(&s->mu, NULL);
    s->lfd = -1;
    s->snap.rssi = -1;
    s->snap.level_db = -120.0f;
    if (port <= 0)
        return s;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        inet_pton(AF_INET, bind_addr && bind_addr[0] ? bind_addr : "0.0.0.0", &sa.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        log_warn("status: cannot serve the status page on %s:%d: %s — continuing without it",
                 bind_addr, port, strerror(errno));
        if (fd >= 0) close(fd);
        return s;
    }
    s->lfd = fd;
    if (pthread_create(&s->thread, NULL, http_main, s) != 0) {
        close(fd);
        s->lfd = -1;
        return s;
    }
    s->running = true;
    log_info("status: page on http://%s:%d/", bind_addr, port);
    return s;
}

void status_free(status_t *s)
{
    if (!s)
        return;
    if (s->running) {
        atomic_store(&s->stop, true);
        pthread_join(s->thread, NULL);
    }
    if (s->lfd >= 0) close(s->lfd);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

status_snapshot_t *status_lock(status_t *s)
{
    pthread_mutex_lock(&s->mu);
    return &s->snap;
}

void status_unlock(status_t *s) { pthread_mutex_unlock(&s->mu); }

void status_add_opening(status_snapshot_t *d, const status_opening_t *o)
{
    d->history[d->hist_head] = *o;
    d->hist_head = (d->hist_head + 1) % STATUS_HISTORY;
    if (d->hist_len < STATUS_HISTORY) d->hist_len++;
}

void status_add_netover(status_snapshot_t *d, const status_netover_t *o)
{
    d->net_history[d->net_head] = *o;
    d->net_head = (d->net_head + 1) % STATUS_NET_HISTORY;
    if (d->net_len < STATUS_NET_HISTORY) d->net_len++;
}
