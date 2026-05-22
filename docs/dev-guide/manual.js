/*
 * Screen-only progressive enhancement for the manual: highlights the current
 * section in the sidebar TOC as you scroll. The document is fully readable and
 * printable without this script (the PDF build does not run JS).
 */
(function () {
  "use strict";
  var links = Array.prototype.slice.call(document.querySelectorAll("nav.sidebar a[href^='#']"));
  if (!links.length) return;
  var byId = {};
  links.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
  var targets = links
    .map(function (a) { return document.getElementById(a.getAttribute("href").slice(1)); })
    .filter(Boolean);

  function setActive(id) {
    links.forEach(function (a) { a.classList.remove("active"); });
    if (byId[id]) byId[id].classList.add("active");
  }

  if ("IntersectionObserver" in window) {
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (e.isIntersecting) setActive(e.target.id);
      });
    }, { rootMargin: "0px 0px -75% 0px", threshold: 0 });
    targets.forEach(function (t) { observer.observe(t); });
  }

  links.forEach(function (a) {
    a.addEventListener("click", function () { setActive(a.getAttribute("href").slice(1)); });
  });
})();
