const pages = [...document.querySelectorAll("[data-page]")];
const links = [...document.querySelectorAll("#pages a")];
function navigate(focus = false) {
  const id = location.hash.slice(1);
  const page = pages.find((p) => p.dataset.page === id) || pages[0];
  for (const section of pages) section.hidden = section !== page;
  for (const link of links) {
    if (link.hash === "#" + page.dataset.page) link.setAttribute("aria-current", "page");
    else link.removeAttribute("aria-current");
  }
  const notice = document.getElementById("libraryStatus");
  if (page.dataset.page === "library") {
    // Transfer feedback stays above even a long file list.
    document.getElementById("libraryTools").before(notice);
  } else {
    page.append(notice);
  }
  document.title = page.querySelector("h1").textContent + " — WOW Controller";
  if (focus) {
    document.getElementById("workspace").focus({ preventScroll: true });
    window.scrollTo(0, 0);
  }
  document.dispatchEvent(new CustomEvent("wowpage", { detail: page.dataset.page }));
}
window.addEventListener("hashchange", () => {
  if (location.hash !== "#workspace") navigate(true);
});
navigate();
