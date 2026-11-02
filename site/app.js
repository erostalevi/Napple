// Landing page behavior: wires the download buttons to the latest GitHub release and swaps in real screenshots when present.
(function () {
  "use strict";
  var DMG = "Napple-Studio.dmg";

  // owner/repo: <meta name="napple-repo">, else https://<owner>.github.io/<repo>/ , else a ?repo=owner/name override for testing.
  function repo() {
    var m = document.querySelector('meta[name="napple-repo"]');
    var q = new URLSearchParams(location.search).get("repo");
    if (q && /^[\w.-]+\/[\w.-]+$/.test(q)) return q;
    if (m && m.content) return m.content;
    var host = location.hostname.match(/^([\w-]+)\.github\.io$/i);
    var seg = location.pathname.split("/").filter(Boolean)[0];
    if (host && seg) return host[1] + "/" + seg;
    return "";
  }
  var R = repo();
  var $ = function (id) { return document.getElementById(id); };

  if (R) {
    var base = "https://github.com/" + R;
    $("dl-dmg").href = base + "/releases/latest/download/" + DMG;
    $("gh-link").href = base;
    $("rel-link").href = base + "/releases";
    document.querySelectorAll("[data-dl-nav]").forEach(function (a) { a.setAttribute("href", "#download"); });

    fetch("https://api.github.com/repos/" + R + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
      .then(function (rel) {
        var dmg = (rel.assets || []).filter(function (a) { return a.name === DMG; })[0];
        var mb = dmg ? " · " + (dmg.size / 1048576).toFixed(0) + " MB" : "";
        var d = rel.published_at ? new Date(rel.published_at).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }) : "";
        $("dl-meta").textContent = (rel.name || rel.tag_name) + mb;
        $("dl-fine").textContent = "Latest build" + (d ? " from " + d : "") + ". Requires an Apple Silicon Mac on macOS 26+. You bring your own game and keys; nothing is bundled.";
      })
      .catch(function () { $("dl-meta").textContent = "Latest build · .dmg"; });
  } else {
    // Unknown repository (custom domain without the meta tag): never leave a dead "#" button.
    ["dl-dmg", "gh-link", "rel-link"].forEach(function (id) { var e = $(id); if (e) { e.removeAttribute("href"); e.setAttribute("aria-disabled", "true"); } });
    $("dl-meta").textContent = "Set the repo in index.html";
  }

  // Online support: online.json is generated from compat/online.json by tools/support/gen_support.py. On failure the static link stays.
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  fetch("online.json")
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (d) {
      var grid = $("online-grid"), modes = d.modes || [];
      grid.textContent = "";
      (d.titles || []).forEach(function (t) {
        var card = el("article", "ocard"), h = el("h3", null, t.name), ul = el("ul"), none = [];
        h.appendChild(el("small", null, t.backend === "none" ? "no online" : t.backend));
        card.appendChild(h);
        modes.forEach(function (m) {
          var s = (t.modes || {})[m.key];
          if (!s) return;
          if (s.status === "unsupported") { none.push(m.label); return; }
          var li = el("li");
          li.appendChild(el("span", null, m.label));
          li.appendChild(el("em", "st " + s.status, s.status));
          li.appendChild(el("small", null, s.note + " (" + s.checked + ")"));
          ul.appendChild(li);
        });
        if (ul.children.length) card.appendChild(ul);
        if (none.length) card.appendChild(el("p", null, none.length === modes.length ? "No online or local multiplayer in this game."
          : "Not in this game: " + none.join(", ") + "."));
        grid.appendChild(card);
      });
      var dl = $("online-modes");
      modes.forEach(function (m) {
        var w = el("div");
        w.appendChild(el("dt", null, m.label));
        w.appendChild(el("dd", null, m.about));
        dl.appendChild(w);
      });
      dl.hidden = !modes.length;
      $("online-meta").textContent = "Last checked " + d.last_checked + ". works · partial · broken · untested (the game has the mode, not tried on Napple yet).";
    })
    .catch(function () {});

  // Screenshots: assets/screens/<file>. Studio slots fall back to the illustrated window, optional (game) slots disappear.
  var anyGame = false;
  document.querySelectorAll(".shot[data-shot]").forEach(function (fig) {
    var optional = fig.hasAttribute("data-optional");
    var probe = new Image();
    probe.onload = function () {
      var img = fig.querySelector("img") || document.createElement("img");
      img.src = probe.src; img.loading = "lazy";
      if (!img.alt) img.alt = (fig.querySelector("figcaption") || {}).textContent || "Napple screenshot";
      var mock = fig.querySelector(".mock"); if (mock) { mock.replaceWith(img); }
      else if (!img.parentNode) fig.prepend(img);
      if (optional) { anyGame = true; $("games").hidden = false; }
    };
    probe.onerror = function () { if (optional) fig.remove(); };
    probe.src = "assets/screens/" + fig.getAttribute("data-shot");
  });
})();
