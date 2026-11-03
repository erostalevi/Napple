// Shared page behaviour: wires the download and GitHub links to the latest release (index.html).
// Every lookup is guarded, so the same script works on the landing page and on the other pages.
(function () {
  "use strict";
  var DMG = "Napple-Studio.dmg";

  // owner/repo: ?repo=owner/name (testing), else <meta name="napple-repo">, else https://<owner>.github.io/<repo>/
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
  function setHref(id, url) { var e = $(id); if (e) e.href = url; }
  function setText(id, text) { var e = $(id); if (e) e.textContent = text; }

  if (R) {
    var base = "https://github.com/" + R;
    setHref("dl-dmg", base + "/releases/latest/download/" + DMG);
    setHref("gh-link", base);
    setHref("gh-hero", base);
    setHref("rel-link", base + "/releases");
    setHref("tp-link", base + "/blob/main/THIRD_PARTY_NOTICES.md");

    if ($("dl-meta")) {
      fetch("https://api.github.com/repos/" + R + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
        .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
        .then(function (rel) {
          var dmg = (rel.assets || []).filter(function (a) { return a.name === DMG; })[0];
          var mb = dmg ? " · " + (dmg.size / 1048576).toFixed(0) + " MB" : "";
          var d = rel.published_at ? new Date(rel.published_at).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }) : "";
          setText("dl-meta", (rel.name || rel.tag_name) + mb);
          setText("dl-fine", "Latest build" + (d ? " from " + d : "") + ". Requires an Apple Silicon Mac on macOS 26+. You bring your own game, keys and firmware; nothing is bundled.");
        })
        .catch(function () { setText("dl-meta", "Latest build · .dmg"); });
    }
  } else {
    // Unknown repository: never leave a dead "#" button.
    ["dl-dmg", "gh-link", "gh-hero", "rel-link"].forEach(function (id) { var e = $(id); if (e) { e.removeAttribute("href"); e.setAttribute("aria-disabled", "true"); } });
    setText("dl-meta", "Set the repo in index.html");
  }
})();
