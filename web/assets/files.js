/* chiral-ftp web UI: the console file manager. Browse the console's storage, download
   files to this device, upload from it (button or drag and drop), and copy,
   move, rename or delete on the console. */
(function () {
  "use strict";

  var $ = function (id) { return document.getElementById(id); };
  var state = { path: "", entries: [], selected: new Set(), jobsFinished: -1, polling: false };
  var onComputer = false;  // set from /api/state
  function dev(onConsole, onThisComputer) { return onComputer ? onThisComputer : onConsole; }

  /* On a computer (platform "host") the page names this machine "this
     computer", not "the console": the console wording is the default in the
     page, and each such element carries its computer version. */
  function applyDevice() {
    if (!onComputer) return;
    document.querySelectorAll("[data-computer]").forEach(function (el) { el.textContent = el.getAttribute("data-computer"); });
    document.querySelectorAll("[data-computer-title]").forEach(function (el) { el.title = el.getAttribute("data-computer-title"); });
  }


  /* ---------- helpers ---------- */

  function encode(params) {
    return Object.keys(params || {}).map(function (k) {
      return encodeURIComponent(k) + "=" + encodeURIComponent(params[k]);
    }).join("&");
  }

  async function call(path, params, method) {
    var res;
    if (method === "GET") {
      var q = encode(params);
      res = await fetch(path + (q ? "?" + q : ""));
    } else {
      res = await fetch(path, {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: encode(params)
      });
    }
    var data = await res.json();
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
    var units = ["KB", "MB", "GB", "TB"], v = n, i = -1;
    do { v /= 1024; i++; } while (v >= 1024 && i < 3);
    return (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + " " + units[i];
  }

  function fmtDate(t) {
    if (t == null || t < 0) return "";
    var d = new Date(t * 1000), p = function (x) { return (x < 10 ? "0" : "") + x; };
    return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " + p(d.getHours()) + ":" + p(d.getMinutes());
  }

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  var toastTimer;
  function toast(msg, err) {
    var t = $("toast");
    t.textContent = msg;
    t.className = "toast" + (err ? " err" : "");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.className = "toast hidden"; }, err ? 6000 : 3000);
  }

  function status(msg) { $("status-text").textContent = msg || ""; }

  /* ---------- listing ---------- */

  async function showDrives() {
    try {
      var data = await call("/api/local/roots", {}, "GET");
      state.path = "";
      state.entries = data.roots.map(function (r) {
        return { name: r.path, label: r.label, dir: 1, drive: 1, size: r.free, mtime: -1 };
      });
      state.selected.clear();
      render();
    } catch (e) { toast(e.message, true); }
  }

  async function open(path, keep) {
    try {
      var data = await call("/api/local/list", { path: path }, "GET");
      if (!keep || data.path !== state.path) state.selected.clear();
      state.path = data.path;
      state.free = data.free;
      state.entries = data.entries.slice().sort(function (a, b) {
        if (a.dir !== b.dir) return b.dir - a.dir;
        return a.name.localeCompare(b.name, undefined, { sensitivity: "base", numeric: true });
      });
      var names = new Set(state.entries.map(function (e) { return e.name; }));
      state.selected.forEach(function (n) { if (!names.has(n)) state.selected.delete(n); });
      try { localStorage.setItem("chiral-files-path", state.path); } catch (e) { /* private mode */ }
      history.replaceState(null, "", "#" + encodeURI(state.path));
      render();
    } catch (e) { toast(e.message, true); }
  }

  function refresh() { return state.path ? open(state.path, true) : showDrives(); }

  function crumbs() {
    var c = $("crumbs");
    c.innerHTML = "";
    if (!state.path) { c.appendChild(el("span", null, "Choose a drive")); return; }
    var parts = state.path.split("/").filter(Boolean), acc = "";
    var root = el("a", null, "/");
    root.onclick = function () { open("/"); };
    c.appendChild(root);
    parts.forEach(function (p, i) {
      acc += "/" + p;
      var target = acc;
      if (i) c.appendChild(el("span", "sep", "/"));
      var a = el("a", null, p);
      a.onclick = function () { open(target); };
      c.appendChild(a);
    });
  }

  function render() {
    crumbs();
    var tbody = $("rows");
    tbody.innerHTML = "";
    $("btn-up").disabled = !state.path;
    $("btn-mkdir").disabled = $("btn-upload").disabled = !state.path;
    state.entries.forEach(function (e) {
      var tr = el("tr");
      if (state.selected.has(e.name)) tr.className = "selected";

      var tdc = el("td", "c-check");
      if (!e.drive) {
        var cb = el("input");
        cb.type = "checkbox";
        cb.checked = state.selected.has(e.name);
        cb.onclick = function (ev) { ev.stopPropagation(); toggle(e.name); };
        tdc.appendChild(cb);
      }
      tr.appendChild(tdc);

      var tdn = el("td", "c-name");
      var nm = el("div", "fm-name");
      nm.appendChild(el("span", "ico " + (e.dir ? "dir" : "file"), e.dir ? "📁" : "📄"));
      nm.appendChild(el("span", null, e.drive ? e.label : e.name));
      if (e.drive) nm.appendChild(el("span", "drive-label", e.name));
      nm.onclick = function () { activate(e); };
      tdn.appendChild(nm);
      tr.appendChild(tdn);

      tr.appendChild(el("td", "c-size", e.drive ? (e.size >= 0 ? fmtSize(e.size) + " free" : "") : e.dir ? "" : fmtSize(e.size)));
      tr.appendChild(el("td", "c-date", e.drive ? "" : fmtDate(e.mtime)));

      var tdm = el("td", "c-more");
      if (!e.drive) {
        var more = el("button", "btn small icon", "⋯");
        more.type = "button";
        more.title = "Actions";
        more.onclick = function (ev) { ev.stopPropagation(); itemMenu(e, more); };
        tdm.appendChild(more);
      }
      tr.appendChild(tdm);
      tbody.appendChild(tr);
    });
    $("empty").classList.toggle("hidden", state.entries.length > 0);
    var files = state.entries.filter(function (e) { return !e.dir; });
    var bytes = files.reduce(function (s, e) { return s + Math.max(0, e.size || 0); }, 0);
    status(state.path ? files.length + " files, " + (state.entries.length - files.length) + " folders · " + fmtSize(bytes) +
      (state.free >= 0 ? " · " + fmtSize(state.free) + " free" : "") : "");
    selbar();
  }

  function activate(e) {
    if (e.drive) return open(e.name);
    if (e.dir) return open(join(state.path, e.name));
    download([join(state.path, e.name)]);
  }

  function toggle(name) {
    if (state.selected.has(name)) state.selected.delete(name);
    else state.selected.add(name);
    render();
  }

  function selbar() {
    var n = state.selected.size;
    $("selbar").classList.toggle("hidden", n === 0);
    $("sel-count").textContent = n + " selected";
    $("check-all").checked = n > 0 && n === state.entries.filter(function (e) { return !e.drive; }).length;
    var onlyFiles = Array.from(state.selected).every(function (name) {
      var e = state.entries.find(function (x) { return x.name === name; });
      return e && !e.dir;
    });
    $("sel-download").disabled = !onlyFiles;
    $("sel-download").title = onlyFiles ? "" : "Folders can't be downloaded; select files";
  }

  function selectedPaths() {
    return Array.from(state.selected).map(function (n) { return join(state.path, n); });
  }

  /* ---------- actions ---------- */

  function download(paths) {
    // One hidden link per file; browsers ask once to allow several downloads.
    paths.forEach(function (p, i) {
      setTimeout(function () {
        var a = document.createElement("a");
        a.href = "/api/fs/download?" + encode({ path: p });
        a.download = "";
        document.body.appendChild(a);
        a.click();
        a.remove();
      }, i * 400);
    });
  }

  async function mkdir() {
    var name = prompt("New folder name", "New folder");
    if (!name) return;
    if (name.indexOf("/") >= 0) return toast("A name can't contain /", true);
    try {
      await call("/api/local/mkdir", { path: join(state.path, name) });
      toast("Created " + name);
      refresh();
    } catch (e) { toast(e.message, true); }
  }

  async function renameItem(e) {
    var name = prompt("Rename \"" + e.name + "\" to", e.name);
    if (!name || name === e.name) return;
    if (name.indexOf("/") >= 0) return toast("A name can't contain /", true);
    try {
      await call("/api/local/rename", { from: join(state.path, e.name), to: join(state.path, name) });
      refresh();
    } catch (err) { toast(err.message, true); }
  }

  // deep: a folder and everything inside it.
  async function chmodItem(e, deep) {
    var cur = e.perm && e.perm.length === 4 ? e.perm.slice(1) : "644";
    var mode = prompt((deep ? "Permissions for \"" + e.name + "\" and everything inside (octal, e.g. 755; folders keep x wherever there is r)"
                            : "Permissions for \"" + e.name + "\" (octal, e.g. 755)"), cur);
    if (!mode) return;
    if (!/^[0-7]{3,4}$/.test(mode)) return toast("Use an octal mode like 755", true);
    try {
      await call("/api/local/chmod", { path: join(state.path, e.name), mode: mode, recursive: deep ? 1 : 0 });
      refresh();
    } catch (err) { toast(err.message, true); }
  }

  async function remove(paths) {
    var what = paths.length === 1 ? "\"" + paths[0].split("/").pop() + "\"" : paths.length + " items";
    if (!confirm("Permanently delete " + what + dev(" from the console", " from this computer") + "? Folders are deleted with everything inside.")) return;
    try {
      await call("/api/local/delete", { paths: paths.join("\n") });
      toast("Deleted " + what);
    } catch (e) { toast(e.message, true); }
    state.selected.clear();
    refresh();
  }

  var menuOpen = null;
  function closeMenu() { $("menu").classList.add("hidden"); menuOpen = null; }

  function itemMenu(e, anchor) {
    var m = $("menu");
    m.innerHTML = "";
    var path = join(state.path, e.name);
    var items = [];
    if (e.dir) items.push(["Open", function () { open(path); }]);
    else items.push(["Download", function () { download([path]); }]);
    items.push(["Rename", function () { renameItem(e); }]);
    items.push(["Copy to…", function () { pick([path], false); }]);
    items.push(["Move to…", function () { pick([path], true); }]);
    items.push(["Change permissions", function () { chmodItem(e, false); }]);
    if (e.dir && !e.link) items.push(["Change permissions recursively", function () { chmodItem(e, true); }]);
    items.push(["Delete", function () { remove([path]); }, true]);
    items.forEach(function (it) {
      var b = el("button", it[2] ? "danger" : null, it[0]);
      b.type = "button";
      b.onclick = function () { closeMenu(); it[1](); };
      m.appendChild(b);
    });
    var r = anchor.getBoundingClientRect();
    m.classList.remove("hidden");
    var left = Math.min(r.right - m.offsetWidth, window.innerWidth - m.offsetWidth - 8);
    var top = r.bottom + 4;
    if (top + m.offsetHeight > window.innerHeight - 8) top = r.top - m.offsetHeight - 4;
    m.style.left = Math.max(8, left) + "px";
    m.style.top = Math.max(8, top) + "px";
    menuOpen = m;
  }

  /* ---------- copy / move: folder picker ---------- */

  var picker = { path: "/", paths: [], move: false };

  async function pickerOpen(path) {
    try {
      var list = $("pick-list");
      var data;
      if (!path) {
        data = await call("/api/local/roots", {}, "GET");
        picker.path = "";
        list.innerHTML = "";
        data.roots.forEach(function (r) { list.appendChild(pickerItem(r.label + "  (" + r.path + ")", r.path)); });
      } else {
        data = await call("/api/local/list", { path: path }, "GET");
        picker.path = data.path;
        list.innerHTML = "";
        data.entries.filter(function (e) { return e.dir; })
          .sort(function (a, b) { return a.name.localeCompare(b.name, undefined, { sensitivity: "base" }); })
          .forEach(function (e) { list.appendChild(pickerItem(e.name, join(data.path, e.name))); });
        if (!list.children.length) list.appendChild(el("li", "muted", "No folders here"));
      }
      $("pick-path").textContent = picker.path || "Choose a drive";
      $("pick-ok").disabled = !picker.path;
    } catch (e) { toast(e.message, true); }
  }

  function pickerItem(label, path) {
    var li = el("li");
    li.appendChild(el("span", "ico", "📁"));
    li.appendChild(el("span", null, label));
    li.onclick = function () { pickerOpen(path); };
    return li;
  }

  function pick(paths, move) {
    picker.paths = paths;
    picker.move = move;
    $("picker-title").textContent = (move ? "Move " : "Copy ") + (paths.length === 1 ? "\"" + paths[0].split("/").pop() + "\"" : paths.length + " items") + " to";
    $("pick-ok").textContent = move ? "Move here" : "Copy here";
    $("picker").classList.remove("hidden");
    pickerOpen(state.path || "");
  }

  async function pickConfirm() {
    $("picker").classList.add("hidden");
    try {
      await call("/api/fs/copy", { paths: picker.paths.join("\n"), target: picker.path, move: picker.move ? 1 : 0 });
      state.selected.clear();
      render();
      pollJobs();
    } catch (e) { toast(e.message, true); }
  }

  /* ---------- copy progress ---------- */

  async function pollJobs() {
    if (state.polling) return;
    state.polling = true;
    try {
      for (;;) {
        var j = await call("/api/fs/jobs", {}, "GET");
        if (state.jobsFinished >= 0 && j.finished !== state.jobsFinished) {
          if (j.failed && j.error && j.finished - state.jobsFinished === 1) toast(j.error, true);
          refresh();
        }
        state.jobsFinished = j.finished;
        var show = j.running || j.queued;
        $("job").classList.toggle("hidden", !show);
        if (!show) break;
        var pct = j.total > 0 ? Math.min(100, Math.round(j.done * 100 / j.total)) : 0;
        $("job-bar").style.width = pct + "%";
        $("job-text").textContent = (j.move ? "Moving " : "Copying ") + j.current + " to " + j.target + " · " + pct + "% · " +
          fmtSize(j.done) + " of " + fmtSize(j.total) + (j.speed > 0 ? " · " + fmtSize(j.speed) + "/s" : "") +
          (j.queued ? " · " + j.queued + " more waiting" : "");
        await new Promise(function (r) { setTimeout(r, 700); });
      }
    } catch (e) { /* the server went away */ }
    state.polling = false;
  }

  /* ---------- uploads from this device ---------- */

  var uploadQueue = [], uploading = false;

  function queueUploads(files) {
    if (!state.path) return toast(dev("Open a folder on the console first", "Open a folder first"), true);
    Array.prototype.forEach.call(files, function (f) {
      var row = el("div", "fm-up");
      var bar = el("div", "bar");
      var fill = el("div");
      bar.appendChild(fill);
      var label = el("span", null, f.name + " · waiting");
      row.appendChild(bar);
      row.appendChild(label);
      $("uploads").appendChild(row);
      uploadQueue.push({ file: f, dir: state.path, row: row, fill: fill, label: label });
    });
    $("uploads").classList.remove("hidden");
    if (!uploading) nextUpload();
  }

  function nextUpload() {
    var u = uploadQueue.shift();
    if (!u) {
      uploading = false;
      setTimeout(function () {
        if (!uploading) {
          $("uploads").querySelectorAll(".fm-up.done").forEach(function (r) { r.remove(); });
          if (!$("uploads").children.length) $("uploads").classList.add("hidden");
        }
      }, 4000);
      return;
    }
    uploading = true;
    var xhr = new XMLHttpRequest();
    var started = Date.now();
    xhr.open("PUT", "/api/fs/upload?" + encode({ dir: u.dir, name: u.file.name, overwrite: $("replace").checked ? 1 : 0 }));
    xhr.upload.onprogress = function (ev) {
      if (!ev.lengthComputable) return;
      var pct = Math.round(ev.loaded * 100 / ev.total);
      var secs = (Date.now() - started) / 1000;
      u.fill.style.width = pct + "%";
      u.label.textContent = u.file.name + " · " + pct + "% · " + fmtSize(ev.loaded) + " of " + fmtSize(ev.total) +
        (secs > 0.5 ? " · " + fmtSize(ev.loaded / secs) + "/s" : "");
    };
    xhr.onload = function () {
      var data = {};
      try { data = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
      if (xhr.status === 200 && data.ok) {
        u.row.classList.add("done");
        u.fill.style.width = "100%";
        u.label.textContent = data.path.split("/").pop() + " · uploaded (" + fmtSize(data.size) + ")";
        if (u.dir === state.path) refresh();
      } else {
        u.row.classList.add("failed");
        u.label.textContent = u.file.name + " · failed: " + (data.error || "HTTP " + xhr.status);
      }
      nextUpload();
    };
    xhr.onerror = function () {
      u.row.classList.add("failed");
      u.label.textContent = u.file.name + " · failed: connection lost";
      nextUpload();
    };
    xhr.send(u.file);
  }

  /* ---------- wiring ---------- */

  $("btn-drives").onclick = showDrives;
  $("btn-up").onclick = function () { if (state.path) open(state.path === "/" ? "/" : parent(state.path)); };
  $("btn-refresh").onclick = refresh;
  $("btn-mkdir").onclick = mkdir;
  $("btn-upload").onclick = function () { $("file-input").click(); };
  $("file-input").onchange = function () { queueUploads(this.files); this.value = ""; };
  $("check-all").onclick = function () {
    if (this.checked) state.entries.forEach(function (e) { if (!e.drive) state.selected.add(e.name); });
    else state.selected.clear();
    render();
  };
  $("sel-download").onclick = function () { download(selectedPaths()); };
  $("sel-copy").onclick = function () { pick(selectedPaths(), false); };
  $("sel-move").onclick = function () { pick(selectedPaths(), true); };
  $("sel-delete").onclick = function () { remove(selectedPaths()); };
  $("sel-clear").onclick = function () { state.selected.clear(); render(); };
  $("pick-up").onclick = function () { pickerOpen(picker.path && picker.path !== "/" ? parent(picker.path) : ""); };
  $("pick-ok").onclick = pickConfirm;
  $("job-cancel").onclick = function () { call("/api/fs/cancel", {}).catch(function () {}); };
  document.querySelectorAll("[data-close]").forEach(function (b) {
    b.onclick = function () { $("picker").classList.add("hidden"); };
  });
  document.addEventListener("click", function (ev) { if (menuOpen && !menuOpen.contains(ev.target)) closeMenu(); });
  document.addEventListener("keydown", function (ev) {
    if (ev.key === "Escape") { closeMenu(); $("picker").classList.add("hidden"); }
    if (ev.key === "Backspace" && document.activeElement === document.body && state.path) $("btn-up").click();
  });

  // Drag and drop files from this device onto the page.
  var dragDepth = 0;
  function hasFiles(ev) { return ev.dataTransfer && Array.prototype.indexOf.call(ev.dataTransfer.types, "Files") >= 0; }
  document.addEventListener("dragenter", function (ev) {
    if (!hasFiles(ev)) return;
    ev.preventDefault();
    dragDepth++;
    $("drop-target").textContent = state.path || "a folder (open one first)";
    $("drop").classList.remove("hidden");
  });
  document.addEventListener("dragover", function (ev) { if (hasFiles(ev)) ev.preventDefault(); });
  document.addEventListener("dragleave", function () {
    if (--dragDepth <= 0) { dragDepth = 0; $("drop").classList.add("hidden"); }
  });
  document.addEventListener("drop", function (ev) {
    if (!hasFiles(ev)) return;
    ev.preventDefault();
    dragDepth = 0;
    $("drop").classList.add("hidden");
    queueUploads(ev.dataTransfer.files);
  });

  // Start in the folder from the address (files.html#/switch/x), else the last
  // one. A remembered path has to still look like a path: an older build
  // stored what the server resolved, which on a Switch came back with a
  // device prefix ("sdmc:/..."), and feeding that back gives "Invalid path"
  // on every load until someone clears the browser's storage.
  var last = location.hash.length > 1 ? decodeURI(location.hash.slice(1)) : null;
  try { if (!last) last = localStorage.getItem("chiral-files-path"); } catch (e) { /* private mode */ }
  if (last && last.charAt(0) !== "/") {
    last = null;
    try { localStorage.removeItem("chiral-files-path"); } catch (e) { /* private mode */ }
  }
  if (last) open(last).then(function () { if (!state.path) showDrives(); });
  else showDrives();
  pollJobs();
  call("/api/state", {}, "GET").then(function (st) {
    onComputer = st.platform === "host";
    applyDevice();
  }).catch(function () { /* the console wording stays */ });
})();
