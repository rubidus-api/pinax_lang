/* The menu of the web editions of the Pinax spec and manual. MIT (see ../manual/LICENSE-CODE).
   The choices are kept in localStorage: pnx-theme (light, dark; none follows the system), pnx-width (fit; none is
   fixed at 960). */
(function () {
  var root = document.documentElement;
  var btn = document.getElementById("menu-btn");
  var menu = document.getElementById("menu");
  if (!btn || !menu) return;

  function store(key, value) {
    try { if (value) localStorage.setItem(key, value); else localStorage.removeItem(key); } catch (e) {}
  }
  function current(group) {
    var v = group === "theme" ? root.dataset.theme : root.dataset.width;
    return v || (group === "theme" ? "system" : "fixed");
  }
  function mark() {
    menu.querySelectorAll("button[data-set]").forEach(function (b) {
      b.setAttribute("aria-pressed", String(current(b.dataset.set) === b.dataset.value));
    });
  }
  function focusables() {
    return Array.prototype.filter.call(menu.querySelectorAll("a, button"), function (e) { return !e.disabled; });
  }
  function open() {
    menu.hidden = false;
    btn.setAttribute("aria-expanded", "true");
    mark();
    var first = menu.querySelector("button[aria-pressed=true]") || focusables()[0];
    if (first) first.focus();
  }
  function close(back) {
    if (menu.hidden) return;
    menu.hidden = true;
    btn.setAttribute("aria-expanded", "false");
    if (back) btn.focus();
  }

  btn.addEventListener("click", function () { if (menu.hidden) open(); else close(true); });
  menu.addEventListener("click", function (ev) {
    var b = ev.target.closest("button[data-set]");
    if (!b) return;
    var group = b.dataset.set, value = b.dataset.value;
    if (group === "theme") {
      if (value === "system") delete root.dataset.theme; else root.dataset.theme = value;
      store("pnx-theme", value === "system" ? null : value);
    } else {
      if (value === "fixed") delete root.dataset.width; else root.dataset.width = value;
      store("pnx-width", value === "fixed" ? null : value);
    }
    mark();
  });
  document.addEventListener("keydown", function (ev) {
    if (menu.hidden) return;
    if (ev.key === "Escape") { ev.preventDefault(); close(true); return; }
    if (ev.key !== "Tab") return;
    /* Focus goes round inside the open menu. */
    var list = focusables(), i = list.indexOf(document.activeElement);
    if (!list.length) return;
    ev.preventDefault();
    var next = ev.shiftKey ? (i <= 0 ? list.length - 1 : i - 1) : (i + 1) % list.length;
    list[next].focus();
  });
  document.addEventListener("click", function (ev) {
    if (!menu.hidden && !menu.contains(ev.target) && ev.target !== btn) close(false);
  });
})();
