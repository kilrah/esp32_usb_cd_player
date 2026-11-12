// Report which cached album-info JSONs contain Cyrillic (or other non-Latin) characters.
const fs = require('fs');
const dir = process.argv[2];
for (const f of fs.readdirSync(dir)) {
  if (!f.endsWith('.json')) continue;
  const j = JSON.parse(fs.readFileSync(dir + '/' + f, 'utf8'));
  const texts = [];
  const add = (label, s) => { if (s) texts.push([label, s]); };
  add('album', j.album); add('artist', j.artist);
  (j.tracks || []).forEach((t, i) => { add('t' + (i + 1) + ' title', t.title); add('t' + (i + 1) + ' artist', t.artist); });
  const hits = [];
  for (const [label, s] of texts) {
    for (const ch of s) {
      const cp = ch.codePointAt(0);
      if (cp > 0x7F && !(cp >= 0xA0 && cp <= 0x17F) && !(cp >= 0x2000 && cp <= 0x206F) && cp !== 0x2010) {
        hits.push([label, ch, cp]);
      }
    }
  }
  const byCp = {};
  for (const [label, ch, cp] of hits) { (byCp[cp] = byCp[cp] || { ch, labels: [] }).labels.push(label); }
  const cyr = Object.keys(byCp).map(Number).filter(c => c >= 0x400 && c <= 0x4FF).sort((a, b) => a - b);
  console.log(f.replace('.json', ''), '|', j.artist, '/', j.album,
    '| distinct non-Latin1 codepoints:', Object.keys(byCp).length,
    '| Cyrillic:', cyr.length, cyr.length ? '(' + cyr.map(c => 'U+' + c.toString(16).toUpperCase() + ' ' + byCp[c].ch).join(', ') + ')' : '');
  if (cyr.length) {
    const sample = texts.filter(([, s]) => [...s].some(ch => ch.codePointAt(0) >= 0x400 && ch.codePointAt(0) <= 0x4FF)).slice(0, 3);
    for (const [l, s] of sample) console.log('     ', l, '=', s);
  }
}
