/* chiral-ftp web UI: a FileZilla-style dual-pane FTP client.
   Plain ES2017 so it runs in any console or phone browser without a build step. */
(function () {
  "use strict";

  var $ = function (id) { return document.getElementById(id); };
  var coarse = window.matchMedia && window.matchMedia("(pointer: coarse)").matches;

  var state = {
    connected: false,
    server: null,
    settings: { connections: 5, max_connections: 30, policy: 0, preserve_mtime: 1, retries: 2 },
    prefs: { show_hidden: true, save_pass: true },
    platform: "switch",  // "host" on a computer: see dev()
    sites: [],
    history: [],
    logSeq: 0,
    queueTab: "queued",
    queue: null,
    lastDone: -1,
    activePane: null,
    currentSiteDirs: null
  };

  /* On a computer (platform "host") the page names this machine "this
     computer", not "the console": the console wording is the default in the
     page, and each such element carries its computer version. */
  function dev(onConsole, onThisComputer) { return state.platform === "host" ? onThisComputer : onConsole; }
  function applyDevice() {
    if (state.platform !== "host") return;
    document.querySelectorAll("[data-computer]").forEach(function (el) { el.textContent = el.getAttribute("data-computer"); });
    document.querySelectorAll("[data-computer-title]").forEach(function (el) { el.title = el.getAttribute("data-computer-title"); });
  }

  /* ---------- helpers ---------- */

  function esc(s) {
    return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
    });
  }

  function encodeForm(params) {
    var parts = [];
    Object.keys(params || {}).forEach(function (k) {
      if (params[k] === undefined || params[k] === null) return;
      parts.push(encodeURIComponent(k) + "=" + encodeURIComponent(params[k]));
    });
    return parts.join("&");
  }

  async function api(path, params, method) {
    var opts = { method: method || "POST", cache: "no-store" };
    var url = path;
    if (opts.method === "GET") {
      var qs = encodeForm(params);
      if (qs) url += "?" + qs;
    } else {
      opts.headers = { "Content-Type": "application/x-www-form-urlencoded" };
      opts.body = encodeForm(params);
    }
    var res = await fetch(url, opts);
    var data;
    try {
      data = await res.json();
    } catch (e) {
      throw new Error(dev("Bad response from the console", "Bad response from chiral-ftp") + " (HTTP " + res.status + ")");
    }
    return data;
  }

  /* Like api(), but throws on {ok:false} so callers can use try/catch. */
  async function call(path, params, method) {
    var data = await api(path, params, method);
    if (!data.ok) throw new Error(data.error || "Request failed");
    return data;
  }

  function join(dir, name) {
    return (dir.charAt(dir.length - 1) === "/" ? dir : dir + "/") + name;
  }

  function parent(path) {
    if (!path || path === "/") return "/";
    var p = path.replace(/\/+$/, "");
    var i = p.lastIndexOf("/");
    return i <= 0 ? "/" : p.slice(0, i);
  }

  function fmtSize(n) {
    if (n == null || n < 0) return "";
    if (n < 1024) return n + " B";
    var units = ["KB", "MB", "GB", "TB"];
    var v = n;
    var i = -1;
    do { v /= 1024; i++; } while (v >= 1024 && i < units.length - 1);
    return (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + " " + units[i];
  }

  function fmtSpeed(n) { return n > 0 ? fmtSize(n) + "/s" : ""; }

  function pad(n) { return n < 10 ? "0" + n : "" + n; }

  function fmtDate(t) {
    if (t == null || t < 0) return "";
    var d = new Date(t * 1000);
    return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " +
      pad(d.getHours()) + ":" + pad(d.getMinutes());
  }

  function fmtEta(sec) {
    if (!isFinite(sec) || sec <= 0) return "";
    sec = Math.round(sec);
    var h = Math.floor(sec / 3600), m = Math.floor(sec % 3600 / 60), s = sec % 60;
    return h ? h + ":" + pad(m) + ":" + pad(s) : m + ":" + pad(s);
  }

  function typeOf(e) {
    if (e.dir) return "Folder";
    var i = e.name.lastIndexOf(".");
    return i > 0 ? e.name.slice(i + 1).toUpperCase() + " file" : "File";
  }

  /* "drwxr-xr-x" or "0755" -> "755" */
  function permToOctal(p) {
    if (!p) return "644";
    if (/^0?[0-7]{3,4}$/.test(p)) return p.replace(/^0(?=\d{3})/, "");
    if (p.length < 10) return "644";
    var out = "";
    for (var i = 1; i < 10; i += 3) {
      var v = (p[i] === "r" ? 4 : 0) + (p[i + 1] === "w" ? 2 : 0) +
        (/[xst]/.test(p[i + 2]) ? 1 : 0);
      out += v;
    }
    return out;
  }

  var toastTimer = null;
  function toast(msg, isErr) {
    var t = $("toast");
    t.textContent = msg;
    t.className = "toast" + (isErr ? " err" : "");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.className = "toast hidden"; }, isErr ? 6000 : 3000);
  }

  function errToast(e) { toast(e && e.message ? e.message : String(e), true); }

  /* ---------- modals ---------- */

  var openModals = [];

  function showModal(id) {
    $(id).classList.remove("hidden");
    $("modal-back").classList.remove("hidden");
    openModals.push(id);
  }

  function hideModal(id) {
    $(id).classList.add("hidden");
    openModals = openModals.filter(function (m) { return m !== id; });
    if (!openModals.length) $("modal-back").classList.add("hidden");
    if (id === "ask-modal" && askResolve) {
      var r = askResolve;
      askResolve = null;
      r(null);
    }
  }

  document.addEventListener("click", function (ev) {
    var close = ev.target.closest("[data-close]");
    if (close) hideModal(close.closest(".modal").id);
  });
  $("modal-back").addEventListener("click", function () {
    if (openModals.length) hideModal(openModals[openModals.length - 1]);
  });

  var askResolve = null;

  /* Promise-based replacement for prompt()/confirm(); input:false asks yes/no. */
  function ask(opts) {
    $("ask-title").textContent = opts.title || "";
    $("ask-text").textContent = opts.text || "";
    $("ask-ok").textContent = opts.ok || "OK";
    $("ask-ok").className = "btn " + (opts.danger ? "danger" : "primary");
    var input = $("ask-input");
    input.classList.toggle("hidden", opts.input === false);
    input.value = opts.value || "";
    showModal("ask-modal");
    setTimeout(function () {
      if (opts.input !== false) {
        input.focus();
        var dot = input.value.lastIndexOf(".");
        input.setSelectionRange(0, opts.selectBase && dot > 0 ? dot : input.value.length);
      } else {
        $("ask-ok").focus();
      }
    }, 30);
    return new Promise(function (resolve) { askResolve = resolve; });
  }

  $("ask-form").addEventListener("submit", function (ev) {
    ev.preventDefault();
    var r = askResolve;
    var value = $("ask-input").classList.contains("hidden") ? true : $("ask-input").value;
    askResolve = null;
    hideModal("ask-modal");
    if (r) r(value);
  });

  /* ---------- context menu ---------- */

  function showMenu(x, y, items) {
    var m = $("context-menu");
    m.innerHTML = "";
    items.forEach(function (it) {
      if (it === "-") {
        var s = document.createElement("div");
        s.className = "sep";
        m.appendChild(s);
        return;
      }
      var b = document.createElement("button");
      b.textContent = it.label;
      b.disabled = !!it.disabled;
      b.addEventListener("click", function () { hideMenu(); it.run(); });
      m.appendChild(b);
    });
    m.classList.remove("hidden");
    var r = m.getBoundingClientRect();
    m.style.left = Math.max(4, Math.min(x, window.innerWidth - r.width - 4)) + "px";
    m.style.top = Math.max(4, Math.min(y, window.innerHeight - r.height - 4)) + "px";
  }

  function hideMenu() { $("context-menu").classList.add("hidden"); }

  document.addEventListener("mousedown", function (ev) {
    if (!ev.target.closest("#context-menu")) hideMenu();
    if (!ev.target.closest("#recent-wrap")) $("recent-wrap").classList.remove("open");
  });

  /* ---------- file panes ---------- */

  function Pane(side) {
    this.side = side;
    this.el = $("pane-" + side);
    this.path = "";
    this.entries = [];
    this.view = [];
    this.sel = {};
    this.cursor = -1;
    this.anchor = -1;
    this.sort = { key: "name", desc: false };
    this.filter = "";
    this.loading = 0;
    this.build();
  }

  Pane.prototype.isLocal = function () { return this.side === "local"; };

  Pane.prototype.other = function () { return this.isLocal() ? remotePane : localPane; };

  Pane.prototype.build = function () {
    var local = this.isLocal();
    var self = this;
    var cols = '<th class="c-chk"><input type="checkbox" data-role="all" title="Select all"></th>' +
      '<th data-sort="name">Name</th><th class="c-size" data-sort="size">Size</th>' +
      '<th class="c-type" data-sort="type">Type</th><th class="c-date" data-sort="mtime">Modified</th>' +
      '<th class="c-perm" data-sort="perm">Permissions</th>' +
      (local ? "" : '<th class="c-owner" data-sort="owner">Owner/Group</th>');
    this.el.innerHTML =
      '<div class="pane-head">' +
        '<span class="title">' + (local ? '<span data-computer="This computer">' + dev("Console", "This computer") + "</span> <small>local site</small>"
                                           : "Server <small>remote site</small>") + "</span>" +
        (local ? '<select data-role="roots" title="Drives"></select>' : "") +
        '<form data-role="pathform"><input class="path" data-role="path" spellcheck="false" autocomplete="off"></form>' +
      "</div>" +
      '<div class="pane-tools">' +
        '<button class="btn small icon" data-act="up" title="Parent folder (Backspace)">&#8593;</button>' +
        '<button class="btn small icon" data-act="refresh" title="Refresh (F5)">&#8635;</button>' +
        '<button class="btn small" data-act="mkdir">New folder</button>' +
        '<button class="btn small" data-act="rename" title="F2">Rename</button>' +
        '<button class="btn small danger" data-act="delete" title="Delete">Delete</button>' +
        '<button class="btn small" data-act="chmod" title="Change permissions">Permissions</button>' +
        '<button class="btn small" data-act="chmod-r" title="Change permissions recursively: folders and everything inside">Recursive</button>' +
        '<input class="filter" data-role="filter" placeholder="Filter" spellcheck="false">' +
        '<span class="spacer"></span>' +
        (local ? '<button class="btn small xfer up" data-act="transfer" title="Upload selection to the server">Upload &#8594;</button>'
               : '<button class="btn small xfer down" data-act="transfer" title="Download selection to ' + dev("the console", "this computer") + '">&#8592; Download</button>') +
      "</div>" +
      '<div class="list-wrap" data-role="wrap"><table class="ftable"><thead><tr>' + cols +
        '</tr></thead><tbody data-role="rows"></tbody></table></div>' +
      '<div class="pane-status" data-role="status"></div>' +
      '<div class="overlay hidden" data-role="overlay"></div>';

    this.q = function (role) { return self.el.querySelector('[data-role="' + role + '"]'); };
    this.rows = this.q("rows");

    this.q("pathform").addEventListener("submit", function (ev) {
      ev.preventDefault();
      self.load(self.q("path").value.trim() || "/");
    });
    this.q("filter").addEventListener("input", function () {
      self.filter = this.value.toLowerCase();
      self.render();
    });
    if (local) {
      this.q("roots").addEventListener("change", function () {
        if (this.value) self.load(this.value);
      });
    }
    this.el.querySelector(".pane-tools").addEventListener("click", function (ev) {
      var b = ev.target.closest("[data-act]");
      if (b && !b.disabled) self.action(b.getAttribute("data-act"));
    });
    this.el.querySelector("thead").addEventListener("click", function (ev) {
      var th = ev.target.closest("[data-sort]");
      if (ev.target.getAttribute("data-role") === "all") {
        self.selectAll(ev.target.checked);
        return;
      }
      if (!th) return;
      var key = th.getAttribute("data-sort");
      if (self.sort.key === key) self.sort.desc = !self.sort.desc;
      else self.sort = { key: key, desc: false };
      self.render();
    });

    this.rows.addEventListener("click", function (ev) {
      var tr = ev.target.closest("tr[data-i]");
      if (!tr) return;
      var i = +tr.getAttribute("data-i");
      if (ev.target.type === "checkbox") {
        self.toggle(i, ev.target.checked);
        self.cursor = self.anchor = i;
        self.renderSel();
        return;
      }
      /* Two taps on the same row open it: controller cursors can't double-click. */
      if (coarse && self.cursor === i && self.selectedCount() === 1 && self.sel[self.view[i].name]) {
        self.open(self.view[i]);
        return;
      }
      self.click(i, ev);
    });
    this.rows.addEventListener("dblclick", function (ev) {
      var tr = ev.target.closest("tr[data-i]");
      if (tr && ev.target.type !== "checkbox") self.open(self.view[+tr.getAttribute("data-i")]);
    });
    this.el.addEventListener("mousedown", function () { setActive(self); });
    this.el.addEventListener("contextmenu", function (ev) {
      ev.preventDefault();
      setActive(self);
      var tr = ev.target.closest("tr[data-i]");
      if (tr) {
        var i = +tr.getAttribute("data-i");
        if (!self.sel[self.view[i].name]) self.click(i, {});
      }
      self.menu(ev.clientX, ev.clientY);
    });

    /* Drag rows to the other pane to transfer them. */
    this.rows.addEventListener("dragstart", function (ev) {
      var tr = ev.target.closest("tr[data-i]");
      if (!tr) return;
      var i = +tr.getAttribute("data-i");
      if (!self.sel[self.view[i].name]) self.click(i, {});
      ev.dataTransfer.effectAllowed = "copy";
      ev.dataTransfer.setData("text/plain", "ftpc:" + self.side);
    });
    var wrap = this.el;
    wrap.addEventListener("dragover", function (ev) {
      if (dragSource() && dragSource() !== self) {
        ev.preventDefault();
        ev.dataTransfer.dropEffect = "copy";
        wrap.classList.add("drop");
      }
    });
    wrap.addEventListener("dragleave", function (ev) {
      if (!wrap.contains(ev.relatedTarget)) wrap.classList.remove("drop");
    });
    wrap.addEventListener("drop", function (ev) {
      wrap.classList.remove("drop");
      var src = dragSource();
      dragging = null;
      if (!src || src === self) return;
      ev.preventDefault();
      var tr = ev.target.closest("tr[data-i]");
      var target = self.path;
      if (tr) {
        var e = self.view[+tr.getAttribute("data-i")];
        if (e.dir) target = join(self.path, e.name);
      }
      src.transfer(target);
    });
    this.rows.addEventListener("dragend", function () { dragging = null; });
    this.rows.addEventListener("dragstart", function () { dragging = self; });
  };

  var dragging = null;
  function dragSource() { return dragging; }

  Pane.prototype.setOverlay = function (html) {
    var o = this.q("overlay");
    o.innerHTML = html || "";
    o.classList.toggle("hidden", !html);
  };

  Pane.prototype.setBusy = function (on) {
    this.loading += on ? 1 : -1;
    this.el.classList.toggle("busy", this.loading > 0);
  };

  Pane.prototype.load = async function (path, keepSel) {
    if (!this.isLocal() && !state.connected) return;
    var prev = keepSel ? Object.assign({}, this.sel) : {};
    this.setBusy(true);
    try {
      var data = await call(this.isLocal() ? "/api/local/list" : "/api/remote/list",
        { path: path }, "GET");
      var changed = data.path !== this.path;
      this.path = data.path;
      this.entries = data.entries;
      this.sel = {};
      if (!changed) {
        var names = {};
        this.entries.forEach(function (e) { names[e.name] = 1; });
        Object.keys(prev).forEach(function (n) { if (names[n]) this.sel[n] = 1; }, this);
      }
      if (changed) {
        this.cursor = -1;
        this.q("filter").value = "";
        this.filter = "";
      }
      this.q("path").value = this.path;
      if (this.isLocal()) this.syncRoot();
      this.setOverlay(this.entries.length || this.filter ? "" : '<div><div class="big">Empty folder</div></div>');
      this.render();
    } catch (e) {
      errToast(e);
      if (!this.path) this.q("path").value = path;
    } finally {
      this.setBusy(false);
    }
  };

  Pane.prototype.refresh = function () { if (this.path) return this.load(this.path, true); };

  Pane.prototype.syncRoot = function () {
    var sel = this.q("roots");
    var best = "";
    var path = this.path;
    Array.prototype.forEach.call(sel.options, function (o) {
      if (o.value && (path === o.value || path.indexOf(o.value === "/" ? "/" : o.value + "/") === 0) &&
          o.value.length > best.length) best = o.value;
    });
    sel.value = best;
  };

  Pane.prototype.sorted = function () {
    var key = this.sort.key;
    var dir = this.sort.desc ? -1 : 1;
    var f = this.filter;
    var list = this.entries.filter(function (e) { return !f || e.name.toLowerCase().indexOf(f) >= 0; });
    list.sort(function (a, b) {
      if (a.dir !== b.dir) return a.dir ? -1 : 1;
      var r = 0;
      if (key === "size") r = (a.size || 0) - (b.size || 0);
      else if (key === "mtime") r = (a.mtime || 0) - (b.mtime || 0);
      else if (key === "type") r = typeOf(a).localeCompare(typeOf(b));
      else if (key === "perm") r = String(a.perm).localeCompare(String(b.perm));
      else if (key === "owner") r = String(a.owner).localeCompare(String(b.owner));
      if (!r) r = a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" });
      return r * dir;
    });
    return list;
  };

  Pane.prototype.render = function () {
    var local = this.isLocal();
    var self = this;
    this.view = this.sorted();
    var html = [];
    for (var i = 0; i < this.view.length; i++) {
      var e = this.view[i];
      var sel = !!this.sel[e.name];
      html.push('<tr data-i="' + i + '" draggable="true" class="' + (sel ? "sel" : "") +
        (i === this.cursor ? " cursor" : "") + '">' +
        '<td class="c-chk"><input type="checkbox"' + (sel ? " checked" : "") + "></td>" +
        '<td><div class="name"><span class="ico' + (e.dir ? " dir" : "") + '">' +
          (e.dir ? "&#128193;" : "&#128196;") + '</span><span class="n" title="' + esc(e.name) + '">' + esc(e.name) + "</span>" +
          (e.link ? '<span class="link">' + (e.target ? "&#8594; " + esc(e.target) : "link") + "</span>" : "") +
        "</div></td>" +
        '<td class="c-size">' + (e.dir ? "" : esc(fmtSize(e.size))) + "</td>" +
        '<td class="c-type">' + esc(typeOf(e)) + "</td>" +
        '<td class="c-date">' + esc(fmtDate(e.mtime)) + "</td>" +
        '<td class="c-perm">' + esc(e.perm) + "</td>" +
        (local ? "" : '<td class="c-owner">' + esc([e.owner, e.group].filter(Boolean).join(" ")) + "</td>") +
        "</tr>");
    }
    this.rows.innerHTML = html.join("");
    this.el.querySelectorAll("th[data-sort]").forEach(function (th) {
      var on = th.getAttribute("data-sort") === self.sort.key;
      th.classList.toggle("sorted", on);
      th.classList.toggle("desc", on && self.sort.desc);
    });
    this.renderStatus();
    this.updateTools();
  };

  /* Updates selection classes without rebuilding the table. */
  Pane.prototype.renderSel = function () {
    var trs = this.rows.children;
    for (var i = 0; i < trs.length; i++) {
      var sel = !!this.sel[this.view[i].name];
      trs[i].classList.toggle("sel", sel);
      trs[i].classList.toggle("cursor", i === this.cursor);
      trs[i].firstChild.firstChild.checked = sel;
    }
    if (this.cursor >= 0 && trs[this.cursor] && trs[this.cursor].scrollIntoView) {
      var wrap = this.q("wrap");
      var tr = trs[this.cursor];
      if (tr.offsetTop < wrap.scrollTop + 30 || tr.offsetTop + tr.offsetHeight > wrap.scrollTop + wrap.clientHeight) {
        tr.scrollIntoView({ block: "nearest" });
      }
    }
    this.renderStatus();
    this.updateTools();
  };

  Pane.prototype.renderStatus = function () {
    var files = 0, dirs = 0, bytes = 0, sf = 0, sd = 0, sb = 0, self = this;
    this.entries.forEach(function (e) {
      if (e.dir) dirs++; else { files++; bytes += Math.max(0, e.size || 0); }
      if (self.sel[e.name]) {
        if (e.dir) sd++; else { sf++; sb += Math.max(0, e.size || 0); }
      }
    });
    var text = sf + sd ?
      "Selected " + sf + " file" + (sf === 1 ? "" : "s") + " and " + sd + " folder" + (sd === 1 ? "" : "s") + ". Total size: " + fmtSize(sb) :
      files + " file" + (files === 1 ? "" : "s") + " and " + dirs + " folder" + (dirs === 1 ? "" : "s") + ". Total size: " + fmtSize(bytes);
    if (this.isLocal()) {
      var opt = this.q("roots").selectedOptions && this.q("roots").selectedOptions[0];
      if (opt && opt.getAttribute("data-free")) text += "  ·  Free: " + opt.getAttribute("data-free");
    }
    this.q("status").textContent = text;
    var all = this.el.querySelector('[data-role="all"]');
    all.checked = this.view.length > 0 && sf + sd === this.entries.length;
  };

  Pane.prototype.updateTools = function () {
    var n = this.selectedCount();
    var off = !this.isLocal() && !state.connected;
    var self = this;
    this.el.querySelectorAll(".pane-tools [data-act]").forEach(function (b) {
      var act = b.getAttribute("data-act");
      var folders = self.selected().some(function (e) { return e.dir && !e.link; });
      var need = { rename: n === 1, delete: n > 0, chmod: n > 0, "chmod-r": folders, transfer: n > 0 && state.connected };
      b.disabled = off || (act in need && !need[act]) || (act === "up" && self.path === "/");
    });
  };

  Pane.prototype.selectedCount = function () { return Object.keys(this.sel).length; };

  Pane.prototype.selected = function () {
    var sel = this.sel;
    return this.entries.filter(function (e) { return sel[e.name]; });
  };

  Pane.prototype.toggle = function (i, on) {
    var name = this.view[i].name;
    if (on) this.sel[name] = 1; else delete this.sel[name];
  };

  Pane.prototype.selectAll = function (on) {
    var self = this;
    this.sel = {};
    if (on) this.view.forEach(function (e) { self.sel[e.name] = 1; });
    this.renderSel();
  };

  Pane.prototype.click = function (i, ev) {
    if (ev.shiftKey && this.anchor >= 0) {
      if (!(ev.ctrlKey || ev.metaKey)) this.sel = {};
      var a = Math.min(this.anchor, i), b = Math.max(this.anchor, i);
      for (var k = a; k <= b; k++) this.toggle(k, true);
    } else if (ev.ctrlKey || ev.metaKey) {
      this.toggle(i, !this.sel[this.view[i].name]);
      this.anchor = i;
    } else {
      this.sel = {};
      this.toggle(i, true);
      this.anchor = i;
    }
    this.cursor = i;
    this.renderSel();
  };

  Pane.prototype.open = function (e) {
    if (!e) return;
    if (e.dir) {
      this.load(join(this.path, e.name));
    } else if (state.connected) {
      /* Like FileZilla: double-clicking a file transfers it. */
      this.sel = {};
      this.sel[e.name] = 1;
      this.transfer(this.other().path);
    }
  };

  Pane.prototype.transfer = async function (target) {
    var items = this.selected();
    if (!items.length) return;
    if (!state.connected) { toast("Connect to a server first", true); return; }
    var other = this.other();
    target = target || other.path;
    if (!target) { toast("Open a folder on the other side first", true); return; }
    var self = this;
    var lines = items.map(function (e) {
      return (e.dir ? "d" : "f") + "\t" + join(self.path, e.name) + "\t" + (e.dir ? -1 : e.size) + "\t" + e.mtime;
    });
    try {
      var data = await call("/api/queue/add", {
        direction: this.isLocal() ? "upload" : "download",
        target: target,
        items: lines.join("\n")
      });
      toast("Added " + data.added + " item" + (data.added === 1 ? "" : "s") + " to the queue");
      setQueueTab("queued");
      poll();
    } catch (e) {
      errToast(e);
    }
  };

  Pane.prototype.action = async function (act) {
    var self = this;
    var api = this.isLocal() ? "/api/local/" : "/api/remote/";
    var items = this.selected();
    try {
      if (act === "up") {
        await this.load(parent(this.path));
      } else if (act === "refresh") {
        await this.refresh();
      } else if (act === "transfer") {
        await this.transfer();
      } else if (act === "mkdir") {
        var name = await ask({ title: "Create folder", text: "Name of the new folder in " + this.path, value: "New folder", ok: "Create" });
        if (!name) return;
        await call(api + "mkdir", { path: join(this.path, name) });
        await this.refresh();
      } else if (act === "rename") {
        if (items.length !== 1) return;
        var to = await ask({ title: "Rename", text: "New name for \"" + items[0].name + "\"", value: items[0].name, ok: "Rename", selectBase: !items[0].dir });
        if (!to || to === items[0].name) return;
        await call(api + "rename", { from: join(this.path, items[0].name), to: join(this.path, to) });
        await this.refresh();
      } else if (act === "delete") {
        if (!items.length) return;
        var what = items.length === 1 ? "\"" + items[0].name + "\"" : items.length + " items";
        var ok = await ask({
          title: "Delete", input: false, danger: true, ok: "Delete",
          text: "Permanently delete " + what + (this.isLocal() ? dev(" from the console", " from this computer") : " from the server") +
            "? Folders are deleted with everything inside them."
        });
        if (!ok) return;
        this.setBusy(true);
        try {
          await call(api + "delete", {
            paths: items.map(function (e) {
              return (self.isLocal() ? "" : (e.dir && !e.link ? "d:" : "f:")) + join(self.path, e.name);
            }).join("\n")
          });
        } finally {
          this.setBusy(false);
        }
        this.sel = {};
        await this.refresh();
      } else if (act === "chmod" || act === "chmod-r") {
        // chmod-r: "Change permissions recursively" -- folders with everything in them.
        if (!items.length) return;
        var recursive = act === "chmod-r";
        var mode = await ask({
          title: recursive ? "Change permissions recursively" : "Change permissions",
          text: recursive ? "Octal mode for " + items.length + " item(s) and everything inside the folders, e.g. 755 or 644. Folders keep x wherever there is r."
                          : "Octal mode for " + items.length + " item(s), e.g. 755 or 644",
          value: permToOctal(items[0].perm), ok: "Apply"
        });
        if (!mode) return;
        if (!/^[0-7]{3,4}$/.test(mode)) throw new Error("Use an octal mode like 755");
        if (recursive) this.setBusy(true);
        try {
          for (var i = 0; i < items.length; i++) {
            var deep = recursive && items[i].dir && !items[i].link;
            await call(api + "chmod", { path: join(this.path, items[i].name), mode: mode, recursive: deep ? 1 : 0 });
          }
        } finally {
          if (recursive) this.setBusy(false);
        }
        await this.refresh();
      }
    } catch (e) {
      errToast(e);
      if (act !== "up" && act !== "refresh") this.refresh();
    }
  };

  Pane.prototype.menu = function (x, y) {
    var self = this;
    var n = this.selectedCount();
    var one = n === 1 ? this.selected()[0] : null;
    var off = !this.isLocal() && !state.connected;
    showMenu(x, y, [
      { label: this.isLocal() ? "Upload to server" : dev("Download to the console", "Download to this computer"), disabled: !n || !state.connected, run: function () { self.transfer(); } },
      { label: "Open", disabled: !(one && one.dir), run: function () { self.open(one); } },
      "-",
      { label: "Create folder", disabled: off, run: function () { self.action("mkdir"); } },
      { label: "Rename", disabled: off || !one, run: function () { self.action("rename"); } },
      { label: "Delete", disabled: off || !n, run: function () { self.action("delete"); } },
      { label: "Change permissions...", disabled: off || !n, run: function () { self.action("chmod"); } },
      { label: "Change permissions recursively...", disabled: off || !this.selected().some(function (e) { return e.dir && !e.link; }),
        run: function () { self.action("chmod-r"); } },
      "-",
      { label: "Copy path", disabled: !one, run: function () { copyText(join(self.path, one.name)); } },
      { label: "Refresh", disabled: off, run: function () { self.refresh(); } }
    ]);
  };

  Pane.prototype.key = function (ev) {
    var k = ev.key;
    var n = this.view.length;
    if (k === "ArrowDown" || k === "ArrowUp") {
      if (!n) return true;
      var i = this.cursor < 0 ? 0 : Math.max(0, Math.min(n - 1, this.cursor + (k === "ArrowDown" ? 1 : -1)));
      this.click(i, { shiftKey: ev.shiftKey });
      return true;
    }
    if (k === "Enter") { this.open(this.view[this.cursor]); return true; }
    if (k === "Backspace") { this.action("up"); return true; }
    if (k === "Delete") { this.action("delete"); return true; }
    if (k === "F2") { this.action("rename"); return true; }
    if (k === "F5") { this.refresh(); return true; }
    if (k === " " && this.cursor >= 0) {
      this.toggle(this.cursor, !this.sel[this.view[this.cursor].name]);
      this.renderSel();
      return true;
    }
    if ((ev.ctrlKey || ev.metaKey) && k.toLowerCase() === "a") { this.selectAll(true); return true; }
    if (k === "ArrowLeft" || k === "ArrowRight" || k === "Tab") { setActive(this.other()); return true; }
    return false;
  };

  function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(function () { toast("Path copied"); }, function () { toast(text); });
    } else {
      toast(text);
    }
  }

  var localPane = new Pane("local");
  var remotePane = new Pane("remote");

  function setActive(p) {
    state.activePane = p;
    localPane.el.classList.toggle("active", p === localPane);
    remotePane.el.classList.toggle("active", p === remotePane);
  }

  document.addEventListener("keydown", function (ev) {
    if (ev.key === "Escape") {
      hideMenu();
      if (openModals.length) hideModal(openModals[openModals.length - 1]);
      return;
    }
    if (openModals.length) return;
    var tag = (ev.target.tagName || "").toLowerCase();
    if (tag === "input" || tag === "textarea" || tag === "select") return;
    var p = state.activePane || localPane;
    if (p.key(ev)) ev.preventDefault();
  });

  /* ---------- connection ---------- */

  function defaultPort(protocol) { return protocol === "sftp" ? 22 : 21; }

  function serverParams(s) {
    return {
      protocol: s.protocol || "ftp",
      keyfile: s.protocol === "sftp" ? (s.keyfile || "") : "",
      host: s.host,
      port: s.port || defaultPort(s.protocol),
      user: s.anonymous ? "" : (s.user || ""),
      pass: s.anonymous ? "" : (s.pass || ""),
      pasv_mode: s.pasv_mode || 0,
      show_hidden: state.prefs.show_hidden ? 1 : 0
    };
  }

  /* A connection the page didn't make itself -- quick connect, the app's
     window, an earlier visit -- also fills the Quickconnect bar, so the top
     line names it. Not the password: quick connect's is a one-time login. */
  function fillQuickbar(server) {
    if (!server) return;
    $("q-proto").value = server.protocol || "ftp";
    $("q-host").value = server.host || "";
    $("q-user").value = server.user || "";
    $("q-port").value = server.port || "";
  }

  /* The server already shown? By address and port only: the page keeps a
     connection it made itself as typed (an empty user, say), so comparing
     more would take it for a new one and send the listing back to "/". */
  function sameServer(shown, now) {
    return !!(state.connected && shown && now && shown.host === now.host && String(shown.port) === String(now.port));
  }

  function setConnected(on, server) {
    state.connected = on;
    state.server = on ? server : null;
    $("btn-disconnect").disabled = !on;
    var sb = $("sb-conn");
    sb.className = "dot" + (on ? "" : " off");
    sb.textContent = on ? "Connected to " + (server.protocol || "ftp") + "://" + (server.user ? server.user + "@" : "") +
      server.host + ":" + server.port : "Not connected";
    var sftp = on && server.protocol === "sftp";
    $("cmd").disabled = sftp;
    $("cmd").placeholder = sftp ? "Custom commands are only available on FTP connections" :
      "Enter a custom FTP command (e.g. SITE HELP, or KILL/MTRW/SELF on ftpsrv)";
    if (!on) {
      remotePane.entries = [];
      remotePane.path = "";
      remotePane.q("path").value = "";
      remotePane.render();
      remotePane.setOverlay('<div><div class="big">Not connected</div>Use Quickconnect or the Site Manager to log in to an FTP server,<br>' +
        "for example your Mac or Linux laptop over SFTP (Remote Login, port 22), FileZilla Server on Windows,<br>" +
        "a NAS, or another Switch running ftpd (FTP, port 5000).</div>");
    } else {
      remotePane.setOverlay("");
    }
    localPane.updateTools();
    remotePane.updateTools();
  }

  var connecting = false;

  async function connect(site, fromHistory) {
    if (connecting) return;
    if (!site.host) { toast("Enter a host name or IP address", true); return; }
    connecting = true;
    $("btn-connect").disabled = true;
    $("btn-connect").textContent = "Connecting...";
    remotePane.setOverlay('<div><div class="big">Connecting to ' + esc(site.host) + "...</div></div>");
    remotePane.setBusy(true);
    try {
      var params = serverParams(site);
      var data = await call("/api/connect", params);
      /* chiral-ftp switches to SFTP by itself when an FTP port answers with SSH. */
      if (data.protocol && data.protocol !== params.protocol) {
        toast("That port is an SSH server, so connected with SFTP");
        site = Object.assign({}, site, { protocol: data.protocol, port: data.port });
      }
      setConnected(true, { host: params.host, port: data.port || params.port, user: params.user, protocol: data.protocol || params.protocol });
      if ($("q-host").value.trim() === site.host) {
        $("q-proto").value = data.protocol || params.protocol;
        $("q-port").value = data.port || params.port;
      }
      addHistory(site);
      await remotePane.load(site.remote_dir || data.path || "/");
      if (site.local_dir) localPane.load(site.local_dir);
      setActive(remotePane);
      if (window.innerWidth <= 820) setMobile("remote");
    } catch (e) {
      setConnected(false);
      errToast(e);
    } finally {
      connecting = false;
      remotePane.setBusy(false);
      $("btn-connect").disabled = false;
      $("btn-connect").textContent = "Quickconnect";
    }
  }

  $("quick").addEventListener("submit", function (ev) {
    ev.preventDefault();
    var host = $("q-host").value.trim();
    var proto = $("q-proto").value;
    var m = /^(s?ftp):\/\//i.exec(host);
    if (m) {
      proto = m[1].toLowerCase();
      $("q-proto").value = proto;
    }
    connect({
      protocol: proto,
      host: host,
      port: parseInt($("q-port").value, 10) || 0,
      user: $("q-user").value.trim(),
      pass: $("q-pass").value,
      pasv_mode: 0
    });
  });
  $("q-proto").addEventListener("change", function () {
    $("q-port").placeholder = defaultPort(this.value);
    var p = $("q-port").value;
    if (p === "21" || p === "22") $("q-port").value = defaultPort(this.value);
  });

  $("btn-disconnect").addEventListener("click", async function () {
    try { await call("/api/disconnect"); } catch (e) { errToast(e); }
    setConnected(false);
  });

  /* ---------- history (Recent) ---------- */

  function siteKey(s) {
    return (s.protocol || "ftp") + "://" + (s.user || "") + "@" + s.host + ":" + (s.port || defaultPort(s.protocol));
  }

  function siteLabel(s) {
    return (s.protocol === "sftp" ? "sftp://" : "") + (s.user ? s.user + "@" : "") + s.host + ":" + (s.port || defaultPort(s.protocol));
  }

  function addHistory(site) {
    var entry = {
      protocol: site.protocol || "ftp",
      keyfile: site.keyfile || "",
      host: site.host,
      port: site.port || defaultPort(site.protocol),
      user: site.anonymous ? "" : (site.user || ""),
      pass: state.prefs.save_pass && !site.anonymous ? (site.pass || "") : "",
      anonymous: !!site.anonymous,
      pasv_mode: site.pasv_mode || 0,
      remote_dir: site.remote_dir || "",
      local_dir: site.local_dir || "",
      name: site.name || "",
      time: Math.floor(Date.now() / 1000)
    };
    var key = siteKey(entry);
    state.history = [entry].concat(state.history.filter(function (h) { return siteKey(h) !== key; })).slice(0, 30);
    saveStore("history", state.history);
    renderRecent();
  }

  function renderRecent() {
    var m = $("recent-menu");
    if (!state.history.length) {
      m.innerHTML = '<button disabled>No recent connections yet</button>';
      return;
    }
    m.innerHTML = state.history.map(function (h, i) {
      return '<button data-h="' + i + '">' + esc(h.name || siteLabel(h)) +
        '<div class="sub">' + esc((h.name ? siteLabel(h) + " · " : "") + fmtDate(h.time)) + "</div></button>";
    }).join("") + '<div class="sep"></div><button data-act="save">Save last connection as site...</button>' +
      '<button data-act="clear">Clear history</button>';
  }

  $("btn-recent").addEventListener("click", function () { $("recent-wrap").classList.toggle("open"); });
  $("recent-menu").addEventListener("click", async function (ev) {
    var b = ev.target.closest("button");
    if (!b) return;
    $("recent-wrap").classList.remove("open");
    if (b.hasAttribute("data-h")) {
      var h = state.history[+b.getAttribute("data-h")];
      $("q-proto").value = h.protocol || "ftp";
      $("q-host").value = h.host;
      $("q-port").value = h.port;
      $("q-user").value = h.user;
      $("q-pass").value = h.pass;
      if (!h.pass && h.user && !h.anonymous && !h.keyfile) {
        var p = await ask({ title: "Password", text: "Password for " + h.user + "@" + h.host });
        if (p === null) return;
        $("q-pass").value = p;
        connect(Object.assign({}, h, { pass: p }));
        return;
      }
      connect(h);
    } else if (b.getAttribute("data-act") === "clear") {
      state.history = [];
      saveStore("history", state.history);
      renderRecent();
    } else if (b.getAttribute("data-act") === "save" && state.history[0]) {
      var site = Object.assign({ id: newId() }, state.history[0]);
      site.name = site.name || site.host;
      state.sites.push(site);
      await saveStore("sites", state.sites);
      openSites(site.id);
    }
  });

  /* ---------- Site Manager ---------- */

  var currentSite = null;

  function newId() { return Date.now().toString(36) + Math.random().toString(36).slice(2, 7); }

  function siteForm() { return $("site-form"); }

  function fillSiteForm(s) {
    var f = siteForm();
    currentSite = s ? s.id : null;
    ["name", "host", "port", "user", "pass", "keyfile", "local_dir", "remote_dir", "comment"].forEach(function (k) {
      f.elements[k].value = s && s[k] != null ? s[k] : "";
    });
    f.elements.pasv_mode.value = s ? String(s.pasv_mode || 0) : "0";
    f.elements.protocol.value = s && s.protocol === "sftp" ? "sftp" : "ftp";
    syncProtocol();
    f.elements.anonymous.checked = !!(s && s.anonymous);
    syncAnon();
    Array.prototype.forEach.call(f.elements, function (el) { el.disabled = !s; });
  }

  function syncAnon() {
    var f = siteForm();
    var anon = f.elements.anonymous.checked;
    f.elements.user.disabled = anon;
    f.elements.pass.disabled = anon;
  }
  siteForm().elements.anonymous.addEventListener("change", syncAnon);

  /* SFTP has no anonymous login or passive mode; FTP has no key files. */
  function syncProtocol() {
    var f = siteForm();
    var sftp = f.elements.protocol.value === "sftp";
    f.querySelectorAll(".sftp-only").forEach(function (el) { el.classList.toggle("hidden", !sftp); });
    f.querySelectorAll(".ftp-only").forEach(function (el) { el.classList.toggle("hidden", sftp); });
    f.elements.anonymous.closest("label").classList.toggle("hidden", sftp);
    if (sftp) f.elements.anonymous.checked = false;
    f.elements.port.placeholder = defaultPort(f.elements.protocol.value);
    syncAnon();
  }
  siteForm().elements.protocol.addEventListener("change", function () {
    var p = siteForm().elements.port;
    if (p.value === "21" || p.value === "22") p.value = defaultPort(this.value);
    syncProtocol();
  });

  function readSiteForm() {
    var f = siteForm();
    return {
      id: currentSite || newId(),
      name: f.elements.name.value.trim() || f.elements.host.value.trim(),
      protocol: f.elements.protocol.value,
      keyfile: f.elements.keyfile.value.trim(),
      host: f.elements.host.value.trim(),
      port: parseInt(f.elements.port.value, 10) || defaultPort(f.elements.protocol.value),
      anonymous: f.elements.anonymous.checked,
      user: f.elements.user.value.trim(),
      pass: f.elements.pass.value,
      pasv_mode: parseInt(f.elements.pasv_mode.value, 10) || 0,
      local_dir: f.elements.local_dir.value.trim(),
      remote_dir: f.elements.remote_dir.value.trim(),
      comment: f.elements.comment.value
    };
  }

  function renderSites() {
    $("site-list").innerHTML = state.sites.length ? state.sites.map(function (s) {
      return '<li data-id="' + esc(s.id) + '" class="' + (s.id === currentSite ? "sel" : "") + '">' + esc(s.name || s.host) +
        "<small>" + esc((s.protocol === "sftp" ? "sftp://" : "") + (s.anonymous ? "anonymous" : s.user || "anonymous") + "@" +
        s.host + ":" + (s.port || defaultPort(s.protocol))) + "</small></li>";
    }).join("") : '<li class="muted"><small>No saved sites. Press "New site".</small></li>';
    $("site-del").disabled = $("site-dup").disabled = !currentSite;
  }

  function openSites(id) {
    var s = state.sites.filter(function (x) { return x.id === id; })[0] || state.sites[0] || null;
    fillSiteForm(s);
    renderSites();
    showModal("sites-modal");
  }

  async function saveCurrentSite() {
    var s = readSiteForm();
    if (!s.host) throw new Error("Enter a host for this site");
    var idx = -1;
    state.sites.forEach(function (x, i) { if (x.id === s.id) idx = i; });
    if (idx >= 0) state.sites[idx] = s; else state.sites.push(s);
    currentSite = s.id;
    await saveStore("sites", state.sites);
    renderSites();
    return s;
  }

  $("btn-sites").addEventListener("click", function () { openSites(currentSite); });
  $("site-list").addEventListener("click", function (ev) {
    var li = ev.target.closest("li[data-id]");
    if (!li) return;
    var s = state.sites.filter(function (x) { return x.id === li.getAttribute("data-id"); })[0];
    fillSiteForm(s);
    renderSites();
  });
  $("site-list").addEventListener("dblclick", function (ev) {
    if (ev.target.closest("li[data-id]")) siteForm().requestSubmit ? siteForm().requestSubmit() : siteForm().dispatchEvent(new Event("submit"));
  });
  $("site-new").addEventListener("click", function () {
    var s = { id: newId(), name: "New site", protocol: "ftp", host: "", port: 21, pasv_mode: 0 };
    state.sites.push(s);
    fillSiteForm(s);
    renderSites();
    siteForm().elements.name.select();
  });
  $("site-dup").addEventListener("click", async function () {
    if (!currentSite) return;
    var s = Object.assign({}, readSiteForm(), { id: newId() });
    s.name += " (copy)";
    state.sites.push(s);
    fillSiteForm(s);
    try { await saveStore("sites", state.sites); } catch (e) { errToast(e); }
    renderSites();
  });
  $("site-del").addEventListener("click", async function () {
    if (!currentSite) return;
    var ok = await ask({ title: "Delete site", text: "Delete this saved site?", input: false, danger: true, ok: "Delete" });
    if (!ok) return;
    state.sites = state.sites.filter(function (x) { return x.id !== currentSite; });
    fillSiteForm(state.sites[0] || null);
    try { await saveStore("sites", state.sites); } catch (e) { errToast(e); }
    renderSites();
  });
  $("site-save").addEventListener("click", async function () {
    try { await saveCurrentSite(); toast("Site saved"); } catch (e) { errToast(e); }
  });
  siteForm().addEventListener("submit", async function (ev) {
    ev.preventDefault();
    try {
      var s = await saveCurrentSite();
      hideModal("sites-modal");
      connect(s);
    } catch (e) {
      errToast(e);
    }
  });

  /* ---------- stores ---------- */

  async function loadStore(name) {
    try {
      var data = await api("/api/store", { name: name }, "GET");
      return Array.isArray(data) || (data && typeof data === "object" && !("ok" in data)) ? data : null;
    } catch (e) {
      return null;
    }
  }

  async function saveStore(name, value) {
    await call("/api/store", { name: name, data: JSON.stringify(value) });
  }

  /* ---------- settings ---------- */

  function fillSettings() {
    var f = $("settings-form");
    f.elements.connections.max = state.settings.max_connections || 30;
    f.elements.connections.value = state.settings.connections;
    $("conn-value").textContent = state.settings.connections;
    f.elements.policy.value = String(state.settings.policy);
    f.elements.retries.value = state.settings.retries;
    f.elements.preserve_mtime.checked = !!state.settings.preserve_mtime;
    f.elements.show_hidden.checked = !!state.prefs.show_hidden;
    f.elements.save_pass.checked = !!state.prefs.save_pass;
  }

  $("btn-settings").addEventListener("click", function () { fillSettings(); showModal("settings-modal"); });
  $("settings-form").elements.connections.addEventListener("input", function () {
    $("conn-value").textContent = this.value;
  });
  $("settings-form").addEventListener("submit", async function (ev) {
    ev.preventDefault();
    var f = this;
    try {
      var data = await call("/api/settings", {
        connections: f.elements.connections.value,
        policy: f.elements.policy.value,
        retries: f.elements.retries.value,
        preserve_mtime: f.elements.preserve_mtime.checked ? 1 : 0
      });
      state.settings = data.settings;
      state.prefs.show_hidden = f.elements.show_hidden.checked;
      state.prefs.save_pass = f.elements.save_pass.checked;
      if (!state.prefs.save_pass) {
        state.history.forEach(function (h) { h.pass = ""; });
        await saveStore("history", state.history);
      }
      await saveStore("prefs", [state.prefs]);
      hideModal("settings-modal");
      toast("Settings saved · " + state.settings.connections + " simultaneous transfer" + (state.settings.connections === 1 ? "" : "s"));
    } catch (e) {
      errToast(e);
    }
  });
  $("btn-forget-keys").addEventListener("click", async function () {
    var ok = await ask({ title: "Forget host keys", text: "Forget every saved SFTP server key? The next connection to each server trusts its current key again.", input: false, ok: "Forget" });
    if (!ok) return;
    try { await call("/api/hostkeys/forget"); toast("Saved host keys forgotten"); } catch (e) { errToast(e); }
  });
  $("btn-exit").addEventListener("click", async function () {
    var ok = await ask({ title: "Stop web server", text: "Stop the web server in chiral-ftp? Transfers keep running in the app" + dev(" on the console.", "."), input: false, danger: true, ok: "Stop" });
    if (!ok) return;
    try { await call("/api/exit"); } catch (e) { /* the server is going away */ }
    document.body.innerHTML = '<div class="empty" style="padding-top:20vh">The web server has stopped. Start it again from chiral-ftp' +
                              dev(" on the console.", ".") + "</div>";
  });

  /* ---------- message log ---------- */

  var logEl = $("log");
  var LOG_MAX = 1500;

  function appendLog(entries) {
    if (!entries.length) return;
    var stick = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 8;
    var frag = document.createDocumentFragment();
    entries.forEach(function (e) {
      var d = new Date(e.time * 1000);
      var div = document.createElement("div");
      div.className = "log-line " + e.kind;
      var label = { status: "Status:", command: "Command:", response: "Response:", error: "Error:" }[e.kind];
      div.innerHTML = '<span class="t">' + pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds()) +
        '</span><span class="c">' + (e.conn ? "#" + e.conn : "&nbsp;&nbsp;") + "</span>" + esc(label + " " + e.text);
      frag.appendChild(div);
    });
    logEl.appendChild(frag);
    while (logEl.childNodes.length > LOG_MAX) logEl.removeChild(logEl.firstChild);
    if (stick) logEl.scrollTop = logEl.scrollHeight;
  }

  $("cmd-form").addEventListener("submit", async function (ev) {
    ev.preventDefault();
    var cmd = $("cmd").value.trim();
    if (!cmd) return;
    if (/^(LIST|NLST|MLSD|RETR|STOR|APPE|STOU)\b/i.test(cmd)) {
      toast("Commands that need a data connection aren't supported here; use the panes instead", true);
      return;
    }
    try {
      await call("/api/remote/command", { cmd: cmd });
      $("cmd").value = "";
      poll();
      if (/^(MKD|RMD|DELE|RNTO|SITE|CHMOD|CWD)\b/i.test(cmd)) remotePane.refresh();
    } catch (e) {
      errToast(e);
    }
  });
  $("btn-log-clear").addEventListener("click", function () {
    logEl.innerHTML = "";
    api("/api/log/clear");
  });
  $("btn-log-toggle").addEventListener("click", function () {
    var c = $("log-pane").classList.toggle("collapsed");
    this.innerHTML = c ? "&#9660;" : "&#9650;";
  });

  /* ---------- queue ---------- */

  function setQueueTab(tab) {
    state.queueTab = tab;
    document.querySelectorAll("#queue-tabs button").forEach(function (b) {
      b.classList.toggle("active", b.getAttribute("data-tab") === tab);
    });
    $("btn-clear").textContent = tab === "queued" ? "Clear list" : "Clear " + (tab === "done" ? "successful" : "failed");
  }

  $("queue-tabs").addEventListener("click", function (ev) {
    var b = ev.target.closest("button[data-tab]");
    if (!b) return;
    setQueueTab(b.getAttribute("data-tab"));
    poll();
  });

  function statusText(it) {
    switch (it.state) {
    case "running":
      var parts = ["#" + it.slot];
      if (it.dir) parts.push("Listing folder...");
      else {
        if (it.speed) parts.push(fmtSpeed(it.speed));
        if (it.speed && it.size > 0) parts.push("ETA " + fmtEta((it.size - it.done) / it.speed));
      }
      return parts.join(" · ");
    case "queued": return it.attempts ? "Retry " + it.attempts : "Queued";
    case "done": return it.dir ? "Folder listed" : "Done";
    case "skipped": return "Skipped: " + it.error;
    case "canceled": return "Canceled";
    default: return it.error || "Failed";
    }
  }

  function renderQueue(q) {
    state.queue = q;
    $("qc-queued").textContent = q.queued;
    $("qc-failed").textContent = q.failed;
    $("qc-done").textContent = q.done;
    $("mt-queue").textContent = q.queued ? "(" + q.queued + ")" : "";
    $("btn-pause").textContent = q.paused ? "Resume queue" : "Pause queue";
    $("btn-retry-all").disabled = !q.failed;
    $("btn-cancel-all").disabled = !q.queued;
    var rows = q.items.map(function (it) {
      var pct = it.size > 0 ? Math.min(100, Math.floor(it.done * 100 / it.size)) : (it.state === "done" ? 100 : 0);
      var up = it.direction === "upload";
      var act = "";
      if (it.state === "running" || it.state === "queued") act = '<button class="btn small icon" data-q="cancel" title="Cancel">&#10005;</button>';
      else if (it.state === "failed" || it.state === "canceled") act = '<button class="btn small icon" data-q="retry" title="Retry">&#8635;</button><button class="btn small icon" data-q="remove" title="Remove">&#10005;</button>';
      else act = '<button class="btn small icon" data-q="remove" title="Remove">&#10005;</button>';
      return '<tr data-id="' + it.id + '">' +
        '<td class="c-dir ' + (up ? "up" : "down") + '" title="' + (up ? "Upload" : "Download") + '">' + (up ? "&#8593;" : "&#8595;") + "</td>" +
        '<td title="' + esc(it.local) + '">' + (it.dir ? "&#128193; " : "") + esc(it.local) + "</td>" +
        '<td title="' + esc(it.host + ":" + it.remote) + '">' + esc(it.remote) + "</td>" +
        '<td class="c-size">' + esc(it.dir ? "" : fmtSize(it.size)) + "</td>" +
        '<td class="c-prog">' + (it.dir ? "" : '<div class="bar"><i style="width:' + pct + '%"></i><span>' + pct + "%" +
          (it.state === "running" && it.size > 0 ? " · " + fmtSize(it.done) : "") + "</span></div>") + "</td>" +
        '<td class="c-status st-' + it.state + '" title="' + esc(it.error) + '">' + esc(statusText(it)) + "</td>" +
        '<td class="c-act">' + act + "</td></tr>";
    });
    $("queue-rows").innerHTML = rows.join("");
    $("queue-empty").classList.toggle("hidden", rows.length > 0);
    $("sb-workers").textContent = q.running ? q.running + " of " + state.settings.connections + " connections busy" :
      "Up to " + state.settings.connections + " simultaneous transfers";
    $("sb-speed").textContent = q.speed ? "Speed " + fmtSpeed(q.speed) + (q.queued_bytes ? " · " + fmtSize(q.queued_bytes) + " left" : "") : "";

    /* Refresh the panes when transfers finish, like FileZilla does. */
    if (state.lastDone >= 0 && q.done !== state.lastDone) scheduleRefresh();
    state.lastDone = q.done;
  }

  var refreshTimer = null;
  function scheduleRefresh() {
    if (refreshTimer) return;
    refreshTimer = setTimeout(function () {
      refreshTimer = null;
      localPane.refresh();
      if (state.connected) remotePane.refresh();
    }, 1500);
  }

  $("queue-rows").addEventListener("click", async function (ev) {
    var b = ev.target.closest("[data-q]");
    if (!b) return;
    var id = b.closest("tr").getAttribute("data-id");
    try {
      await call("/api/queue/" + b.getAttribute("data-q"), { id: id });
      poll();
    } catch (e) { errToast(e); }
  });
  $("btn-pause").addEventListener("click", async function () {
    await call("/api/queue/pause", { paused: state.queue && state.queue.paused ? 0 : 1 }).catch(errToast);
    poll();
  });
  $("btn-retry-all").addEventListener("click", async function () {
    await call("/api/queue/retry", { id: 0 }).catch(errToast);
    setQueueTab("queued");
    poll();
  });
  $("btn-cancel-all").addEventListener("click", async function () {
    var ok = await ask({ title: "Cancel all", text: "Cancel every queued and running transfer?", input: false, danger: true, ok: "Cancel all" });
    if (!ok) return;
    await call("/api/queue/cancel", { id: 0 }).catch(errToast);
    poll();
  });
  $("btn-clear").addEventListener("click", async function () {
    var tab = state.queueTab;
    if (tab === "queued") {
      var ok = await ask({ title: "Clear queue", text: "Cancel running transfers and remove all queued items?", input: false, danger: true, ok: "Clear" });
      if (!ok) return;
      await call("/api/queue/cancel", { id: 0 }).catch(errToast);
      tab = "failed";
    }
    await call("/api/queue/clear", { tab: tab }).catch(errToast);
    poll();
  });

  /* ---------- polling ---------- */

  var pollTimer = null;
  var polling = false;
  var offline = false;

  async function poll() {
    if (polling) return;
    polling = true;
    clearTimeout(pollTimer);
    var delay = 1000;
    try {
      var data = await api("/api/poll", { log: state.logSeq, tab: state.queueTab }, "GET");
      if (offline) {
        offline = false;
        toast(dev("Reconnected to the console", "Reconnected to chiral-ftp"));
      }
      appendLog(data.log);
      state.logSeq = data.log_seq;
      renderQueue(data.queue);
      if (state.connected && !data.connected) {
        var lost = state.drops !== undefined && data.drops !== state.drops, host = state.server ? state.server.host : "the server";
        setConnected(false);
        if (lost) toast("Lost the connection to " + host + ": it stopped answering", true);
      } else if (data.connected && data.server && !connecting && !sameServer(state.server, data.server)) {
        /* Connected from elsewhere -- quick connect opens this page while it
           is still connecting -- or to another server than the one shown:
           show it, top line and listing, without a reload. */
        setConnected(true, data.server);
        fillQuickbar(data.server);
        remotePane.load("");
      }
      state.drops = data.drops;
      if (data.queue.running) delay = 600;
    } catch (e) {
      if (!offline) {
        offline = true;
        toast("Lost contact with chiral-ftp. Retrying...", true);
      }
      delay = 3000;
    } finally {
      polling = false;
      pollTimer = setTimeout(poll, document.hidden ? 4000 : delay);
    }
  }

  /* ---------- mobile layout ---------- */

  function setMobile(pane) {
    $("app").setAttribute("data-mobile", pane);
    document.querySelectorAll("#mobile-tabs button").forEach(function (b) {
      b.classList.toggle("active", b.getAttribute("data-pane") === pane);
    });
  }
  $("mobile-tabs").addEventListener("click", function (ev) {
    var b = ev.target.closest("button[data-pane]");
    if (b) setMobile(b.getAttribute("data-pane"));
  });
  setMobile("local");

  /* ---------- startup ---------- */

  async function loadRoots() {
    try {
      var data = await call("/api/local/roots", {}, "GET");
      var sel = localPane.q("roots");
      sel.innerHTML = '<option value="">Drives</option>' + data.roots.map(function (r) {
        var free = r.free >= 0 ? fmtSize(r.free) : "";
        return '<option value="' + esc(r.path) + '" data-free="' + esc(free) + '">' + esc(r.label) +
          (free ? " (" + free + " free)" : "") + "</option>";
      }).join("");
      return data.roots;
    } catch (e) {
      errToast(e);
      return [];
    }
  }

  async function start() {
    setConnected(false);
    setQueueTab("queued");
    try {
      var st = await call("/api/state", {}, "GET");
      state.settings = st.settings;
      state.platform = st.platform;
      applyDevice();
      $("sb-version").textContent = st.app + " " + st.version;
      if (st.connected) {
        setConnected(true, st.server);
        fillQuickbar(st.server);
      }
    } catch (e) {
      errToast(e);
    }
    var lists = await Promise.all([loadStore("sites"), loadStore("history"), loadStore("prefs")]);
    state.sites = lists[0] || [];
    state.history = lists[1] || [];
    if (lists[2] && lists[2][0]) Object.assign(state.prefs, lists[2][0]);
    renderRecent();
    if (state.history[0] && !state.connected) {  // connected: the bar already names that server
      $("q-proto").value = state.history[0].protocol || "ftp";
      $("q-host").value = state.history[0].host;
      $("q-port").value = state.history[0].port;
      $("q-user").value = state.history[0].user;
      $("q-pass").value = state.history[0].pass || "";
    }

    var roots = await loadRoots();
    localPane.load(roots.length ? roots[0].path : "/");
    if (state.connected) remotePane.load("");
    setActive(localPane);
    poll();
  }

  start();
})();
