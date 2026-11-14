// Landing page: fills the compatibility summary from compat.json (generated). Same-origin only; without JS the link below it still works.
(function () {
  "use strict";
  var box = document.getElementById("compat-mini");
  if (!box) return;
  function el(t, c, x) { var e = document.createElement(t); if (c) e.className = c; if (x != null) e.textContent = x; return e; }
  fetch("compat.json").then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); }).then(function (d) {
    var stages = d.stages || [];
    (d.titles || []).forEach(function (t) {
      var c = el("article", "tcard");
      c.appendChild(el("h3", null, t.name));
      c.appendChild(el("p", "meta", t.title_id));
      var chips = el("div", "chips");
      stages.forEach(function (s) {
        var st = t.stages[s.key] || "n/t", ch = el("span", "chip " + st.replace("/", ""));
        ch.appendChild(document.createTextNode(s.label + " "));
        ch.appendChild(el("i", null, st));
        chips.appendChild(ch);
      });
      c.appendChild(chips);
      box.appendChild(c);
    });
  }).catch(function () {
    box.appendChild(el("p", "fine", "The summary could not be loaded. Open the full compatibility page."));
  });
})();
