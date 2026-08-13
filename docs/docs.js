/* ============================================================================
   NULL SECTOR // DEMO ENGINE — docs runtime
   - builds the sidebar nav from a single config
   - syntax-highlights code blocks (no external deps)
   - copy buttons, mobile nav, active-link highlighting, prev/next pager
   - types the demo.nsd header in the index hero
   ========================================================================== */
(function () {
  "use strict";

  /* --- nav config ---------------------------------------------------------- */
  var NAV = [
    { group: "Overview", items: [
      { href: "index.html", num: "00", label: "Introduction" },
      { href: "cheatsheet.html", num: "⚡", label: "Cheat Sheet" },
      { href: "build.html", num: "01", label: "Build & Run" },
      { href: "troubleshooting.html", num: "⚠", label: "Troubleshooting" }
    ]},
    { group: "Authoring", items: [
      { href: "editor.html", num: "ED", label: "Demo Editor" },
      { href: "dsl.html", num: "02", label: "Scripting DSL" },
      { href: "timeline.html", num: "03", label: "Timeline" },
      { href: "animation.html", num: "04", label: "Animation" },
      { href: "scene.html", num: "05", label: "Scene Graph" },
      { href: "cameras.html", num: "06", label: "Camera Rigs" }
    ]},
    { group: "Rendering", items: [
      { href: "shaders.html", num: "07", label: "Shader Manager" },
      { href: "shader-ai.html", num: "AI", label: "AI Shader Generator" },
      { href: "shadertoy.html", num: "08", label: "Shadertoy Import" },
      { href: "postfx.html", num: "09", label: "Post FX" },
      { href: "assets.html", num: "10", label: "Assets & Reload" },
      { href: "plugins.html", num: "11", label: "Plugins" },
      { href: "first-effect.html", num: "12", label: "First Effect" }
    ]},
    { group: "Distribution", items: [
      { href: "packaging.html", num: "13", label: "VFS & Packaging" }
    ]}
  ];

  var FLAT = [];
  NAV.forEach(function (g) { g.items.forEach(function (i) { FLAT.push(i); }); });

  function currentPage() {
    var p = (location.pathname.split("/").pop() || "index.html").toLowerCase();
    if (p === "" || p === "docs") p = "index.html";
    return p;
  }

  /* --- sidebar ------------------------------------------------------------- */
  var sidebar = document.getElementById("sidebar");
  if (sidebar) {
    var html = '<a class="brand" href="index.html">' +
      '<span class="brand-mark">NS</span>' +
      '<span class="brand-name">NULL SECTOR<br><small>DEMO ENGINE</small></span></a>' +
      '<nav class="nav" aria-label="Documentation">';
    var cur = currentPage();
    NAV.forEach(function (g) {
      html += '<div class="nav-group"><div class="nav-label">' + g.group + "</div>";
      g.items.forEach(function (i) {
        var active = (i.href === cur) ? " active" : "";
        html += '<a class="' + active.trim() + '" href="' + i.href + '">' +
          '<span class="num">' + i.num + "</span><span>" + i.label + "</span></a>";
      });
      html += "</div>";
    });
    html += "</nav>";
    html += '<div class="side-foot">NULL SECTOR &middot; data-driven<br>demo engine &middot; C++17 / GL</div>';
    sidebar.innerHTML = html;

    /* active link inside nested page anchors */
    if (location.hash) {
      var t = document.getElementById(location.hash.slice(1));
      if (t) setTimeout(function () { t.scrollIntoView({ block: "start" }); }, 60);
    }
  }

  /* --- mobile nav ---------------------------------------------------------- */
  var menuBtn = document.getElementById("menu-btn");
  if (menuBtn && sidebar) {
    /* A11y: the off-canvas sidebar is invisible but still in the tab order and
       accessibility tree while closed. Toggle inert + aria-hidden with the open
       state, re-sync on resize so a closed mobile nav never traps keyboard or
       screen-reader users (desktop always keeps it interactive), trap Tab
       inside the open drawer so focus can't fall through to the page behind,
       and return focus to the trigger on every close path. */
    menuBtn.setAttribute("aria-controls", "sidebar");
    /* one source of truth for the breakpoint: matchMedia stays in lockstep
       with the CSS @media (max-width: 1024px) rule, so the JS can never
       drift from the stylesheet */
    var mqMobile = window.matchMedia("(max-width: 1024px)");
    var isMobile = function () { return mqMobile.matches; };
    var isOpen = function () { return sidebar.classList.contains("open"); };
    /* the sidebar markup is built once above, so its focusable set is static */
    var sidebarLinks = sidebar.querySelectorAll("a[href]");

    var syncSidebarA11y = function () {
      var mobile = isMobile();
      var closed = !isOpen();
      var inert = mobile && closed;
      sidebar.inert = inert; /* removes tab order + accessibility tree */
      sidebar.setAttribute("aria-hidden", inert ? "true" : "false");
      if (inert) menuBtn.setAttribute("aria-expanded", "false");
      else if (mobile) menuBtn.setAttribute("aria-expanded", "true");
      else menuBtn.removeAttribute("aria-expanded");
    };

    /* the single close path: restore the ☰ icon, re-sync a11y state, and put
       the keyboard back on the trigger (used by the button, nav links, and
       Escape, so focus always returns no matter how the drawer closes) */
    var closeSidebar = function (returnFocus) {
      sidebar.classList.remove("open");
      menuBtn.textContent = "☰";
      syncSidebarA11y();
      if (returnFocus && isMobile()) menuBtn.focus();
    };

    menuBtn.addEventListener("click", function () {
      if (isOpen()) closeSidebar(true);
      else {
        sidebar.classList.add("open");
        menuBtn.textContent = "✕";
        syncSidebarA11y();
      }
    });
    sidebar.addEventListener("click", function (e) {
      if (e.target.closest("a") && isMobile()) closeSidebar(true);
    });

    /* Focus trap (active only while the drawer is open on mobile). The ring is
       [menu button → brand → … → last nav link]: the button sits directly
       before the drawer in DOM order, so Tab wraps from the last link to the
       brand, Shift+Tab wraps from the button (or the brand) to the last link,
       and a Tab from anywhere outside pulls focus into the drawer — keyboard
       users can never land on the skip link or the page behind the drawer. */
    document.addEventListener("keydown", function (e) {
      if (!(isMobile() && isOpen())) return;
      if (e.key === "Escape") { closeSidebar(true); return; }
      if (e.key !== "Tab") return;
      if (!sidebarLinks.length) { menuBtn.focus(); return; }
      var first = sidebarLinks[0];
      var last = sidebarLinks[sidebarLinks.length - 1];
      var active = document.activeElement;
      var inSidebar = sidebar.contains(active);
      if (e.shiftKey) {
        if (active === first || active === menuBtn || !inSidebar) {
          e.preventDefault();
          last.focus();
        }
      } else if (active === last || !inSidebar) {
        e.preventDefault();
        first.focus();
      }
    });

    window.addEventListener("resize", syncSidebarA11y);
    /* also re-sync once everything has loaded: the layout viewport can settle
       after first paint (tab restore, rotation, split-screen) without a resize */
    window.addEventListener("load", syncSidebarA11y);
    syncSidebarA11y(); /* initial state (e.g. a narrow first paint) */
  }

  /* --- prev/next pager ----------------------------------------------------- */
  var pager = document.getElementById("pager");
  if (pager) {
    var cur = currentPage();
    var idx = -1;
    FLAT.forEach(function (i, n) { if (i.href === cur) idx = n; });
    var out = "";
    if (idx > 0) {
      var p = FLAT[idx - 1];
      out += '<a class="prev" href="' + p.href + '"><span class="p-dir">&larr; prev</span><span class="p-name">' + p.num + " · " + p.label + "</span></a>";
    } else {
      out += '<span></span>';
    }
    if (idx >= 0 && idx < FLAT.length - 1) {
      var n = FLAT[idx + 1];
      out += '<a class="next" href="' + n.href + '"><span class="p-dir">next &rarr;</span><span class="p-name">' + n.num + " · " + n.label + "</span></a>";
    }
    pager.innerHTML = out;
  }

  /* --- syntax highlighting -------------------------------------------------- */
  var KEYWORDS = ["demo","scene","at","camera","show","hide","load","shader","play",
    "fade","transition","post","preset","legacy","anim","marker","speed","loop","jump",
    "mesh","sprite","image","text","light","particles","empty","rig","static","drift","fly",
    "nave","orbit","spiral","hover","city","descend","path","bpm","duration","bars",
    "intensity","chapter","title","music","if","else","for","while","return","break",
    "continue","const","uniform","in","out","inout","struct","true","false","precision",
    "common","buffer_a","buffer_b","buffer_c","buffer_d","image"];

  var TYPES = ["void","float","int","bool","vec2","vec3","vec4","mat2","mat3","mat4",
    "sampler2D","samplerCube","vec","mainImage","texture","texture2D"];

  function esc(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  function highlightLine(line, lang) {
    if (!line) return "";
    var out = "";
    var i = 0;
    var re = /(\/\/.*$)|("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')|(\b\d+(?:\.\d+)?f?\b)|(\b[A-Za-z_][A-Za-z0-9_]*\b)|(#[A-Za-z_]+)/g;
    var m;
    var last = 0;
    while ((m = re.exec(line)) !== null) {
      out += esc(line.slice(last, m.index));
      var tok = m[0];
      if (m[1]) out += '<span class="tok-c">' + esc(tok) + "</span>";
      else if (m[2]) out += '<span class="tok-s">' + esc(tok) + "</span>";
      else if (m[3]) out += '<span class="tok-n">' + esc(tok) + "</span>";
      else if (m[4]) {
        var w = tok;
        if (KEYWORDS.indexOf(w) >= 0) out += '<span class="tok-k">' + w + "</span>";
        else if (TYPES.indexOf(w) >= 0) out += '<span class="tok-t">' + w + "</span>";
        else if (lang === "glsl" && /^[A-Za-z_][A-Za-z0-9_]*\(/.test(line.slice(m.index + tok.length).replace(/^\s+/, ""))) out += '<span class="tok-f">' + w + "</span>";
        else out += esc(w);
      }
      else if (m[5]) out += '<span class="tok-d">' + esc(tok) + "</span>";
      last = m.index + tok.length;
    }
    out += esc(line.slice(last));
    return out;
  }

  document.querySelectorAll(".code pre").forEach(function (pre) {
    var block = pre.closest(".code");
    var lang = block && block.getAttribute("data-lang") || "";
    var text = pre.textContent.replace(/\n$/, "");
    pre.innerHTML = text.split("\n").map(function (l) {
      return '<span class="ln">' + highlightLine(l, lang) + "</span>";
    }).join("\n");
  });

  /* --- copy buttons -------------------------------------------------------- */
  document.querySelectorAll(".code").forEach(function (block) {
    var head = block.querySelector(".code-head");
    if (!head) return;
    var btn = document.createElement("button");
    btn.className = "copy-btn";
    btn.type = "button";
    btn.textContent = "copy";
    btn.setAttribute("aria-label", "Copy code block");
    head.appendChild(btn);
    btn.addEventListener("click", function () {
      var txt = block.querySelector("pre").innerText;
      var done = function () {
        btn.textContent = "copied";
        btn.classList.add("done");
        setTimeout(function () { btn.textContent = "copy"; btn.classList.remove("done"); }, 1600);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(txt).then(done, function () { fallbackCopy(txt); done(); });
      } else { fallbackCopy(txt); done(); }
    });
  });

  function fallbackCopy(text) {
    var ta = document.createElement("textarea");
    ta.value = text;
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand("copy"); } catch (e) {}
    document.body.removeChild(ta);
  }

  /* --- terminal hero (index) ------------------------------------------------ */
  var term = document.getElementById("term");
  if (term) {
    // print: render the full script instantly (the typing animation would
    // otherwise capture a mid-type snapshot on paper)
    var termFull = function () {
      var html = "";
      for (var i = 0; i < lines.length; i++) {
        html += '<span class="' + (lines[i].c || "") + '">' + esc(lines[i].t) + "</span>\n";
      }
      term.innerHTML = html;
    };
    var mq = window.matchMedia && window.matchMedia("print");
    if (mq && mq.addEventListener) mq.addEventListener("change", function (e) { if (e.matches) termFull(); });
    window.addEventListener("beforeprint", termFull);
    var lines = [
      { t: "$ ./ns_demo --demo=data/demo.nsd", c: "dim" },
      { t: "[MAIN] NULL SECTOR // DEMO ENGINE", c: "dim" },
      { t: "[AUDIO] no track file - running silent (drop a WAV/MP3 to add music)", c: "dim" },
      { t: "" },
      { t: "demo \"NULL SECTOR DEMO ENGINE\" {", c: "hl" },
      { t: "    bpm 216", c: "" },
      { t: "    duration 125", c: "" },
      { t: "}", c: "hl" },
      { t: "" },
      { t: "at 0.0   { show Intro;      marker Awakening }", c: "dim" },
      { t: "at 13.3  { show Cathedral;  marker Nave }", c: "dim" },
      { t: "at 26.7  { show Neuralnet;  marker Synapse }", c: "dim" },
      { t: "at 40    { show Machine;    marker Machine }", c: "dim" },
      { t: "at 53.3  { show Voxel;      marker Downtown }", c: "dim" },
      { t: "at 64.4  { show Shadertoy;  marker Imported }", c: "dim" },
      { t: "at 77.8  { show Model;      marker Pipeline }", c: "dim" },
      { t: "at 88.9  { show Ghost;      marker Formation }", c: "dim" },
      { t: "at 102.2 { show Tunnel;     marker Reprise }", c: "dim" },
      { t: "at 115.6 { show Greetings;  marker SignOff }", c: "dim" }
    ];
    var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    if (reduce) {
      term.innerHTML = lines.map(function (l) {
        return '<span class="' + (l.c || "") + '">' + esc(l.t) + "</span>";
      }).join("\n") + '<span class="cursor"></span>';
      return;
    }

    var li = 0, ci = 0;
    term.innerHTML = '<span class="cursor"></span>';

    function render() {
      var shown = [];
      for (var a = 0; a < lines.length; a++) {
        if (a > li) break;
        var txt = (a < li) ? lines[a].t : lines[a].t.slice(0, ci);
        shown.push('<span class="' + (lines[a].c || "") + '">' + esc(txt) + "</span>");
        shown.push("\n");
      }
      term.innerHTML = shown.join("") + '<span class="cursor"></span>';
    }

    (function tick() {
      if (li >= lines.length) return;
      if (ci >= lines[li].t.length) { li++; ci = 0; }
      else { ci++; }
      render();
      setTimeout(tick, 12);
    })();
  }
})();
