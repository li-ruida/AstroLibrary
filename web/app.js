const desktopHost = typeof location !== "undefined" && new URLSearchParams(location.search).get("desktop") === "1";
if (document.documentElement) document.documentElement.classList.toggle("desktop-host", desktopHost);

const state = {
  items: [],
  editedItems: [],
  editedByTarget: {},
  subCollections: {},
  lookup: new Map(),
  facets: { targets: [], devices: [], dates: [], tags: [] },
  settings: null,
  sourceStatus: {},
  editedStatus: {},
  auth: { enabled: false, authenticated: false, username: "" },
  stats: {},
  scan: { lastScanAt: "", durationMs: 0, cached: false },
  galleryMode: "auto",
  mergeTargets: true,
  filters: { target: "all", device: "all", tag: "all", rating: "all", dateFrom: "", dateTo: "", sort: "capture_desc" },
  query: "",
  viewMode: "grid",
  r2Timer: null,
  workspace: "all",
  workflows: {},
  ratingPending: false,
  lightboxItems: [],
  lightboxItem: null,
  detailsRequest: 0,
  fitsOptions: new Map(),
};
const $ = (selector) => document.querySelector(selector);

function fitsOptionKey(item) { return `${state.settings?.source || ""}|${item.url}`; }

function fitsOptions(item) {
  return state.fitsOptions.get(fitsOptionKey(item)) || { mode: "auto", black: 0, brightness: 0, neutralize: false };
}

function previewUrl(item) {
  if (item.kind !== "raw") return item.url;
  const options = fitsOptions(item);
  const query = new URLSearchParams({mode: options.mode, black: String(options.black), brightness: String(options.brightness), neutralize: options.neutralize ? "1" : "0"});
  return `${item.url}${item.url.includes("?") ? "&" : "?"}${query}`;
}

function syncFitsControls(item) {
  const isFits = item.kind === "raw";
  $("#fits-controls").hidden = !isFits;
  $("#lightbox").classList.toggle("has-fits-controls", isFits);
  if (!isFits) return;
  const options = fitsOptions(item);
  $("#fits-mode").value = options.mode;
  $("#fits-black").value = options.black;
  $("#fits-brightness").value = options.brightness;
  $("#fits-neutralize").checked = options.neutralize;
  $("#fits-black-value").textContent = Number(options.black).toFixed(1);
  $("#fits-brightness-value").textContent = Number(options.brightness).toFixed(1);
}

function updateFitsPreview() {
  const item = state.lightboxItem;
  if (!item || item.kind !== "raw") return;
  state.fitsOptions.set(fitsOptionKey(item), {mode: $("#fits-mode").value, black: Number($("#fits-black").value), brightness: Number($("#fits-brightness").value), neutralize: $("#fits-neutralize").checked});
  const url = previewUrl(item);
  // Use the same per-image adjustment in cards, details, SUB and the lightbox.
  document.querySelectorAll("img").forEach((img) => {
    if (img.getAttribute("src")?.split("?")[0] === item.url.split("?")[0]) {
      img.hidden = false;
      img.parentElement.querySelector(".preview-error")?.remove();
      img.src = url;
    }
  });
  syncFitsControls(item);
}

function formatBytes(bytes) {
  if (!bytes) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.floor(Math.log(bytes) / Math.log(1024));
  return `${(bytes / 1024 ** index).toFixed(index > 1 ? 1 : 0)} ${units[index]}`;
}

function escapeHtml(value) {
  return String(value).replace(/[&<>'"]/g, (char) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" }[char]));
}

function updateSectionTitle() {
  const target = state.filters.target;
  $("#section-title").textContent = state.workspace !== "all" ? ({ favorites: "收藏", todo: "待处理", processing: "处理中", done: "已完成" }[state.workspace]) : target === "all" ? "全部照片" : target === "recent" ? "最近拍摄" : target;
}

function imagePriority(item) {
  return item.kind === "raw" ? 0 : item.isEdited ? 1 : 2;
}

function mergeGalleryItems(items) {
  if (!state.mergeTargets) return items;
  const grouped = new Map();
  items.forEach((item) => {
    const current = grouped.get(item.category);
    if (!current || (ratingSort() ? compareRatings(item, current) < 0 :
        imagePriority(item) < imagePriority(current) || (imagePriority(item) === imagePriority(current) && item.capturedAt > current.capturedAt))) {
      grouped.set(item.category, item);
    }
  });
  return [...grouped.values()];
}

function photoRating(item) { return Number.isInteger(item?.rating) && item.rating >= 1 && item.rating <= 5 ? item.rating : 0; }
function ratingSort() { return ["rating_desc", "rating_asc"].includes(state.filters.sort); }
function compareRatings(a, b) {
  const x = photoRating(a), y = photoRating(b);
  if (!x && y) return 1;
  if (x && !y) return -1;
  return (state.filters.sort === "rating_asc" ? x - y : y - x)
    || imagePriority(a) - imagePriority(b) || b.capturedAt.localeCompare(a.capturedAt) || a.id.localeCompare(b.id);
}
function ratingLabel(value) { return value ? `${value} 星` : "未评分"; }
function ratingEditor(item) {
  const rating = photoRating(item);
  return `<div class="rating-buttons" role="group" aria-label="照片评分">${[1,2,3,4,5].map((value) => `<button type="button" class="rating-star ${value <= rating ? "is-rated" : ""}" data-rating-value="${value}" aria-label="${value} 星" aria-pressed="${value === rating}" ${state.ratingPending ? "disabled" : ""}>${value <= rating ? "★" : "☆"}</button>`).join("")}<button type="button" class="rating-clear" data-rating-value="0" ${state.ratingPending || !rating ? "disabled" : ""}>清除评分</button></div><span class="rating-status" aria-live="polite">${ratingLabel(rating)}</span>`;
}
async function saveRating(item, value) {
  if (!item || state.ratingPending) return;
  state.ratingPending = true;
  document.querySelectorAll("[data-rating-value]").forEach((button) => { button.disabled = true; });
  try {
    const data = await postJson("/api/rating", { id: item.id, isEdited: !!item.isEdited, scope: item.ratingScope, rating: value });
    const copies = [...state.items, ...state.editedItems, ...state.lookup.values(), item];
    const relative = item.isEdited ? item.id.slice(7) : item.id;
    copies.forEach((copy) => {
      if ((copy.isEdited ? copy.id.slice(7) : copy.id) === relative && copy.ratingScope === item.ratingScope) copy.rating = data.rating;
    });
    renderGallery();
    showToast(data.rating ? `已评为 ${data.rating} 星` : "已清除评分");
  } catch (error) { showToast(`评分保存失败：${error.message}`); }
  finally {
    state.ratingPending = false;
    const detail = $("#detail-rating");
    if (detail && state.detailsItem) detail.innerHTML = ratingEditor(state.detailsItem);
    if (state.lightboxItem) $("#lightbox-rating").innerHTML = ratingEditor(state.lightboxItem);
  }
}

function workflow(item) {
  const key = item.category.split(" - ", 1)[0].trim();
  return state.workflows[key] || { favorite: false, stage: "todo", notes: "" };
}
const stageLabels = { todo: "待处理", processing: "处理中", done: "已完成" };

function sourceItems() {
  const originals = state.items.filter((item) => ["photo", "raw"].includes(item.kind));
  return state.galleryMode === "source" ? originals : state.galleryMode === "edited" ? state.editedItems : [...state.editedItems, ...originals];
}
function preferredItems(items) {
  if (state.galleryMode === "auto" && !ratingSort()) {
    const best = new Map();
    items.forEach((item) => best.set(item.category, Math.min(best.get(item.category) ?? Infinity, imagePriority(item))));
    items = items.filter((item) => imagePriority(item) === best.get(item.category));
  }
  return mergeGalleryItems(items);
}
function galleryItems() { return preferredItems(sourceItems()); }

function galleryFacets() {
  const counts = { targets: new Map(), devices: new Map(), tags: new Map() };
  for (const item of sourceItems()) {
    for (const [facet, values] of [["targets", [item.category]], ["devices", [item.device]], ["tags", item.tags || []]]) {
      for (const value of values) {
        if (value) counts[facet].set(value, (counts[facet].get(value) || 0) + 1);
      }
    }
  }
  return Object.fromEntries(Object.entries(counts).map(([name, entries]) => [name,
    [...entries].sort(([a], [b]) => a.localeCompare(b, "zh-CN")).map(([value, count]) => ({ value, count }))]));
}

function visibleItems() {
  const query = state.query.trim().toLowerCase();
  const filtered = sourceItems().filter((item) => {
    const w = workflow(item);
    const workspaceMatch = state.workspace === "all" || (state.workspace === "favorites" ? w.favorite : w.stage === state.workspace);
    const targetMatch = ["all", "recent"].includes(state.filters.target) || item.category === state.filters.target;
    const deviceMatch = state.filters.device === "all" || item.device === state.filters.device;
    const tagMatch = state.filters.tag === "all" || (item.tags || []).includes(state.filters.tag);
    const ratingMatch = !state.filters.rating || state.filters.rating === "all" || (state.filters.rating === "0" ? !photoRating(item) : photoRating(item) >= Number(state.filters.rating));
    const fromMatch = !state.filters.dateFrom || item.captureDate >= state.filters.dateFrom;
    const toMatch = !state.filters.dateTo || item.captureDate <= state.filters.dateTo;
    const searchMatch = !query || `${item.name} ${item.category} ${item.device} ${item.captureDate} ${item.extension} ${(item.tags || []).join(" ")} ${w.notes}`.toLowerCase().includes(query);
    return workspaceMatch && targetMatch && deviceMatch && tagMatch && ratingMatch && fromMatch && toMatch && searchMatch;
  });
  const items = preferredItems(filtered);
  return state.filters.target === "recent" ? items.sort((a, b) => b.capturedAt.localeCompare(a.capturedAt)).slice(0, 10) : items;
}

function sortedItems(items) {
  const milkyWayPriority = (item) => /^milky[\s_-]*way/i.test(String(item.category || "").trim()) ? 1 : 0;
  const direction = state.filters.sort === "capture_asc" ? 1 : -1;
  return [...items].sort((a, b) => {
    const targetPriority = milkyWayPriority(a) - milkyWayPriority(b);
    if (targetPriority) return targetPriority;
    if (ratingSort()) return compareRatings(a, b);
    if (state.filters.sort === "target") return a.category.localeCompare(b.category, "zh-CN") || imagePriority(a) - imagePriority(b) || b.capturedAt.localeCompare(a.capturedAt);
    if (state.filters.sort === "device") return a.device.localeCompare(b.device, "zh-CN") || imagePriority(a) - imagePriority(b) || b.capturedAt.localeCompare(a.capturedAt);
    return imagePriority(a) - imagePriority(b) || direction * a.capturedAt.localeCompare(b.capturedAt);
  });
}

function renderSidebar() {
  const photos = galleryItems();
  $("#all-count").textContent = photos.length;
  $("#recent-count").textContent = Math.min(photos.length, 10);
  const counts = {};
  photos.forEach((item) => { counts[item.category] = (counts[item.category] || 0) + 1; });
  $("#target-list").innerHTML = Object.entries(counts).sort(([a], [b]) => a.localeCompare(b, "zh-CN")).map(([name, count]) => `
    <button class="nav-item ${state.filters.target === name ? "is-active" : ""}" data-target="${escapeHtml(name)}"><img class="nav-thumb" src="${escapeHtml(previewUrl(photos.find((item) => item.category === name) || {url: ""}))}" alt="" loading="lazy" /><span>${escapeHtml(name)}</span><b>${count}</b></button>`).join("");
  document.querySelectorAll(".nav-item[data-target]").forEach((button) => {
    button.classList.toggle("is-active", state.workspace === "all" && button.dataset.target === state.filters.target);
  });
  document.querySelectorAll(".workflow-nav").forEach((button) => button.classList.toggle("is-active", button.dataset.workspace === state.workspace));
  const targets = [...new Map([...state.items, ...state.editedItems].filter((item) => !item.isSub).map((item) => [item.category, item])).values()];
  ["favorites", "todo", "processing", "done"].forEach((key) => {
    $("#" + key + "-count").textContent = targets.filter((item) => key === "favorites" ? workflow(item).favorite : workflow(item).stage === key).length;
  });
}

function renderFilters(facets) {
  facets = galleryFacets();
  const fill = (selector, firstLabel, entries) => {
    const select = $(selector);
    const current = select.value || "all";
    select.innerHTML = `<option value="all">${firstLabel}</option>${entries.map((entry) => `<option value="${escapeHtml(entry.value)}">${escapeHtml(entry.value)} · ${entry.count}</option>`).join("")}`;
    select.value = entries.some((entry) => entry.value === current) ? current : "all";
  };
  const targetSelect = $("#target-filter");
  const targetEntries = state.mergeTargets
    ? galleryItems().reduce((entries, item) => entries.some((entry) => entry.value === item.category) ? entries : [...entries, { value: item.category, count: 1 }], [])
    : (facets.targets || []);
  const targetValues = targetEntries.map((entry) => entry.value);
  const allowedTargets = ["all", "recent", ...targetValues];
  targetSelect.innerHTML = `<option value="all">全部目标</option><option value="recent">最近拍摄</option>${targetEntries.map((entry) => `<option value="${escapeHtml(entry.value)}">${escapeHtml(entry.value)} · ${entry.count}</option>`).join("")}`;
  state.filters.target = allowedTargets.includes(state.filters.target) ? state.filters.target : "all";
  targetSelect.value = state.filters.target;
  fill("#device-filter", "全部设备", facets.devices || []);
  fill("#tag-filter", "全部标签", facets.tags || []);
  state.filters.device = $("#device-filter").value;
  state.filters.tag = $("#tag-filter").value;
  $("#gallery-mode").value = state.galleryMode;
  $("#merge-targets").checked = state.mergeTargets;
  $("#target-filter").value = state.filters.target;
  $("#device-filter").value = state.filters.device;
  $("#tag-filter").value = state.filters.tag;
  $("#rating-filter").value = state.filters.rating || "all";
  $("#date-from").value = state.filters.dateFrom;
  $("#date-to").value = state.filters.dateTo;
  $("#sort-filter").value = state.filters.sort;
}

function renderAccess(library = state) {
  const banner = $("#access-banner");
  const edited = state.galleryMode === "edited";
  const status = (edited ? library.editedStatus : library.sourceStatus) || {};
  const label = edited ? "精选成片目录" : "图库目录";
  const hasAlternatePhotos = state.galleryMode === "auto" && state.editedItems.length > 0;
  const needsHelp = !hasAlternatePhotos && (!status.accessible || !status.mediaCount);
  banner.hidden = !needsHelp;
  if (!needsHelp) return;
  const permission = status.permissionRequired || (status.path || "").includes("/Library/Containers/");
  $("#access-title").textContent = permission ? `需要 macOS 授权访问${label}` : status.exists ? `${label}已连接，但没有发现照片` : `请选择有效的${label}`;
  $("#access-message").textContent = permission ? "请点击“选择目录”，在弹出的 macOS 文件夹选择器中选择你自己的照片文件夹；选择一次后会记住权限。若仍被阻止，请在系统设置 → 隐私与安全性中允许 AstroLibrary 访问文件。" : `当前路径：${status.path || "未设置"}。你可以重新扫描，或在设置中选择/填写目录。`;
}

function renderScan(scan = {}) {
  const status = $("#library-status");
  if (!status) return;
  const at = scan.lastScanAt ? new Date(scan.lastScanAt).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }) : "—";
  const label = scan.cached ? "缓存结果 · " + at : "刚刚扫描 · " + (scan.durationMs || 0) + " ms";
  status.innerHTML = '<span class="status-dot"></span><span>' + escapeHtml(label) + '</span>';
  status.title = scan.lastScanAt ? "最近扫描：" + scan.lastScanAt : "尚未完成扫描";
}

function renderHero(stats) {
  const targets = new Set([...state.items, ...state.editedItems].map((item) => item.category));
  $("#library-summary").innerHTML = `<span><strong>${targets.size}</strong> 个目标</span><i>/</i><span><strong>${stats.photos || 0}</strong> 张原始成片</span><i>/</i><span><strong>${stats.raw || 0}</strong> 个 FITS</span><i>/</i><span><strong>${stats.editedPhotos || 0}</strong> 张精选成片</span>`;
}

function resetFilters() {
  state.query = "";
  state.workspace = "all";
  state.filters = { target: "all", device: "all", tag: "all", rating: "all", dateFrom: "", dateTo: "", sort: "capture_desc" };
  $("#search").value = "";
  renderFilters(state.facets); renderSidebar(); updateSectionTitle(); renderGallery();
}

function renderGallery() {
  renderAccess();
  const items = sortedItems(visibleItems());
  $("#gallery").classList.toggle("is-list", state.viewMode === "list");
  $("#result-count").textContent = `${items.length} ${state.mergeTargets ? "个目标" : "张照片"}`;
  document.querySelectorAll("[data-mode]").forEach((button) => {
    button.classList.toggle("is-active", button.dataset.mode === state.galleryMode);
    button.setAttribute("aria-pressed", String(button.dataset.mode === state.galleryMode));
  });
  const active = [state.query && `搜索：${state.query}`, state.filters.device !== "all" && state.filters.device, state.filters.tag !== "all" && state.filters.tag, state.filters.rating && state.filters.rating !== "all" && (state.filters.rating === "0" ? "未评分" : `${state.filters.rating} 星及以上`), state.filters.dateFrom && `自 ${state.filters.dateFrom}`, state.filters.dateTo && `至 ${state.filters.dateTo}`].filter(Boolean);
  $("#active-filters").hidden = !active.length;
  $("#active-filters").innerHTML = `<span>${active.map(escapeHtml).join(" · ")}</span><button type="button" data-reset>清除筛选 ×</button>`;
  if (!items.length) {
    const noLibrary = !state.items.length && !state.editedItems.length;
    const emptyTitle = state.galleryMode === "edited" && !state.editedItems.length ? "还没有精选成片" : noLibrary ? "从第一张星空开始" : state.workspace === "favorites" ? "还没有匹配的收藏" : "没有匹配的照片";
    const emptyMessage = state.galleryMode === "edited" ? "在本地目录与设置中选择精选成片目录，放入处理并挑选好的 JPG、PNG 或 TIFF。" : noLibrary ? "选择你存放照片的任意文件夹，整理每一次观测。" : "试试其他日期或设备，或清除筛选重新浏览。";
    $("#gallery").innerHTML = `<div class="empty-state"><div><span class="empty-orbit">◎</span><h3>${emptyTitle}</h3><p>${emptyMessage}</p><button class="button secondary" ${noLibrary || state.galleryMode === "edited" ? "data-settings" : "data-reset"}>${noLibrary || state.galleryMode === "edited" ? "本地目录与设置" : "清除筛选"}</button></div></div>`;
    return;
  }
  $("#gallery").innerHTML = items.map((item) => {
    const w = workflow(item);
    return `<article class="media-card" data-id="${escapeHtml(item.id)}">
      <button class="photo-preview" data-preview-id="${escapeHtml(item.id)}" aria-label="预览 ${escapeHtml(item.category)}"><img src="${escapeHtml(previewUrl(item))}" loading="lazy" decoding="async" alt="${escapeHtml(item.name)}" /></button>
      <button class="favorite-button ${w.favorite ? "is-favorite" : ""}" data-favorite-id="${escapeHtml(item.id)}" aria-label="${w.favorite ? "取消收藏" : "收藏"} ${escapeHtml(item.category)}" aria-pressed="${w.favorite}">${w.favorite ? "★" : "☆"}</button>
      <div class="card-body"><div class="card-heading"><button class="card-title" data-info-id="${escapeHtml(item.id)}" title="查看照片详情">${escapeHtml(item.category)}</button><span class="stage-label stage-${escapeHtml(w.stage)}">${stageLabels[w.stage] || "待处理"}</span></div><div class="card-meta"><span>${escapeHtml(item.captureDate || item.modified.slice(0, 10))}</span><span>·</span><span>${escapeHtml(item.device || "未知设备")}</span><span class="card-rating" aria-label="评分：${ratingLabel(photoRating(item))}">${photoRating(item) ? `★ ${photoRating(item)}` : "未评分"}</span><span class="card-kind">${item.isEdited ? "修图" : escapeHtml(item.extension.toUpperCase())}</span></div></div>
    </article>`;
  }).join("");
}

async function saveWorkflow(item, changes) {
  if (!item) return false;
  try {
    const data = await postJson("/api/workflow", { target: item.category, changes });
    state.workflows[data.target] = data.workflow;
    renderSidebar(); renderGallery();
    if (state.lightboxItem) updateLightboxControls();
    showToast("已保存到本地图库");
    return true;
  } catch (error) { showToast(`保存失败：${error.message}`); return false; }
}

function renderSettings(settings, sourceStatus) {
  if (!settings) return;
  $("#server-port").value = settings.port || 8765;
  $("#footer-port").textContent = `127.0.0.1:${settings.port || 8765}`;
  $("#export-scope").value = settings.exportScope || "all";
  const authConfigPath = settings.authConfigPath || "~/Library/Application Support/AstroLibrary/settings.json";
  $("#auth-settings-status").textContent = settings.authEnabled
    ? `已启用账号保护 · ${settings.authUsername || "本机用户"}；请通过本机配置工具修改。配置文件：${authConfigPath}`
    : `未启用；网页端不可修改。可通过本机配置工具开启。配置文件：${authConfigPath}`;
  $("#logout").hidden = !state.auth.authenticated;
  $("#source-path").value = settings.libraryPath || settings.source || sourceStatus?.path || "";
  $("#seestar-source").value = settings.seestarSource || "";
  $("#destination-path").value = settings.destination || "";
  $("#edited-path").value = settings.editedSource || "";
  const editedStatus = state.editedStatus.path === settings.editedSource ? state.editedStatus : settings.editedStatus || {};
  $("#edited-status").textContent = editedStatus.path ? `${editedStatus.mediaCount || 0} 张精选成片 · ${editedStatus.reason || ""}` : "尚未配置精选成片目录";
  renderR2Settings(settings.r2 || {});
  const tags = settings.tags || [];
  $("#tag-library").innerHTML = tags.length ? tags.map((tag) => `<button class="tag-chip tag-filter-chip" data-tag="${escapeHtml(tag)}">${escapeHtml(tag)}</button>`).join("") : `<small>还没有自定义标签，在目标详情中添加即可。</small>`;
  document.querySelectorAll(".tag-filter-chip").forEach((button) => button.addEventListener("click", () => {
    state.filters.tag = button.dataset.tag;
    $("#tag-filter").value = state.filters.tag;
    closeSettings();
    renderGallery();
  }));
  renderSeestarCandidates(settings.candidates || []);
}

function renderSeestarCandidates(candidates) {
  $("#source-candidates").innerHTML = candidates.length ? `<small>自动发现的目录</small>${candidates.map((candidate) => `<button class="candidate-row" data-source="${escapeHtml(candidate.path)}"><span><strong>${escapeHtml(candidate.label || "Seestar 目录")}</strong><em>${escapeHtml(candidate.path)}</em></span><b>${candidate.mediaCountKnown ? `${candidate.mediaCountCapped ? "至少 " : ""}${candidate.mediaCount || 0} 个文件` : (candidate.hasEntries ? "已发现" : "空目录")}</b></button>`).join("")}` : `<small>暂未发现候选目录</small>`;
  document.querySelectorAll(".candidate-row").forEach((button) => button.addEventListener("click", () => { $("#seestar-source").value = button.dataset.source; }));
}

function renderR2Settings(r2) {
  $("#r2-enabled").checked = r2.enabled === true;
  $("#r2-auto-upload").checked = r2.autoUpload === true;
  $("#r2-account-id").value = r2.accountId || "";
  $("#r2-bucket").value = r2.bucket || "";
  $("#r2-access-key").value = r2.accessKeyId || "";
  $("#r2-secret-key").value = "";
  $("#r2-secret-key").placeholder = r2.secretConfigured ? "已安全保存；留空则保持不变" : "Secret Access Key";
  $("#r2-public-url").value = r2.publicBaseUrl || "";
  $("#r2-key-prefix").value = r2.keyPrefix || "astrolibrary/previews";
  const status = $("#r2-status");
  status.classList.toggle("is-error", Boolean(r2.lastError));
  status.classList.toggle("is-ok", r2.configured && !r2.lastError && !r2.syncing);
  if (!r2.configured) {
    status.textContent = "尚未配置完整凭据";
  } else if (r2.syncing) {
    status.textContent = `正在同步 · 待处理 ${r2.pending || 0} · 已上传 ${r2.uploaded || 0} · 跳过 ${r2.skipped || 0} · 失败 ${r2.failed || 0}`;
  } else {
    const last = r2.lastSyncAt ? new Date(r2.lastSyncAt).toLocaleString() : "尚未同步";
    status.textContent = r2.lastError
      ? `最近错误：${r2.lastError} · 本地映射 ${r2.mapped || 0} 条`
      : `连接配置已保存 · 本地映射 ${r2.mapped || 0} 条 · ${last} · 映射文件：${r2.mappingPath || "—"}`;
  }
  $("#sync-r2").disabled = !r2.enabled || !r2.configured || r2.syncing;
  $("#test-r2").disabled = r2.syncing;
}

function collectR2Config() {
  return {
    enabled: $("#r2-enabled").checked,
    autoUpload: $("#r2-auto-upload").checked,
    accountId: $("#r2-account-id").value.trim(),
    bucket: $("#r2-bucket").value.trim(),
    accessKeyId: $("#r2-access-key").value.trim(),
    secretAccessKey: $("#r2-secret-key").value,
    publicBaseUrl: $("#r2-public-url").value.trim(),
    keyPrefix: $("#r2-key-prefix").value.trim(),
  };
}

async function refreshR2Status() {
  window.clearTimeout(state.r2Timer);
  try {
    const response = await fetch("/api/settings", { cache: "no-store" });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "读取 R2 状态失败");
    state.settings = data;
    renderR2Settings(data.r2 || {});
    if (data.r2?.syncing) state.r2Timer = window.setTimeout(refreshR2Status, 1200);
    if (!data.r2?.syncing && data.r2?.lastSyncAt) await loadLibrary();
  } catch (error) {
    $("#r2-status").textContent = error.message;
    $("#r2-status").classList.add("is-error");
  }
}

async function testR2Connection() {
  const button = $("#test-r2");
  button.disabled = true;
  $("#r2-status").textContent = "正在连接 Cloudflare R2…";
  try {
    const data = await postJson("/api/r2/test", { r2: collectR2Config() });
    $("#r2-status").textContent = data.message || "R2 连接成功";
    $("#r2-status").classList.remove("is-error");
    $("#r2-status").classList.add("is-ok");
  } catch (error) {
    $("#r2-status").textContent = `连接失败：${error.message}`;
    $("#r2-status").classList.add("is-error");
  } finally {
    button.disabled = false;
  }
}

async function syncR2Now() {
  try {
    const settings = await postJson("/api/settings", { r2: collectR2Config() });
    state.settings = settings;
    $("#r2-secret-key").value = "";
    const result = await postJson("/api/r2/sync");
    showToast(result.started ? "R2 预览同步已在后台启动" : "R2 同步任务已在运行");
    await refreshR2Status();
  } catch (error) {
    showToast(`R2 同步失败：${error.message}`);
    $("#r2-status").textContent = error.message;
    $("#r2-status").classList.add("is-error");
  }
}

function openSettings() {
  if (!$("#settings-panel").classList.contains("is-open")) state.settingsReturnFocus = document.activeElement;
  $("#settings-panel").inert = false;
  renderSettings(state.settings, null);
  $("#settings-panel").classList.add("is-open");
  $("#settings-backdrop").classList.add("is-open");
  $("#settings-panel").setAttribute("aria-hidden", "false");
  $("#settings-backdrop").setAttribute("aria-hidden", "false");
  $("#close-settings").focus();
  refreshR2Status();
}

function closeSettings() {
  const wasOpen = $("#settings-panel").classList.contains("is-open");
  $("#settings-panel").inert = true;
  $("#settings-panel").classList.remove("is-open");
  $("#settings-backdrop").classList.remove("is-open");
  $("#settings-panel").setAttribute("aria-hidden", "true");
  $("#settings-backdrop").setAttribute("aria-hidden", "true");
  if (wasOpen) (state.settingsReturnFocus?.isConnected ? state.settingsReturnFocus : $("#settings")).focus();
}

async function postJson(endpoint, payload = {}) {
  const response = await fetch(endpoint, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-AstroLibrary-CSRF": state.auth.csrfToken || "",
    },
    body: JSON.stringify(payload),
  });
  const data = await response.json();
  if (response.status === 401) {
    showAuthGate(state.auth);
  }
  if (!response.ok || data.ok === false) throw new Error(data.error || "操作失败");
  return data;
}

function showAuthGate(auth = state.auth) {
  state.auth = { ...state.auth, ...auth, enabled: true, authenticated: false };
  $("#auth-gate").hidden = false;
  $("#auth-gate").setAttribute("aria-hidden", "false");
  $("#auth-login-username").value = auth.username || "";
  $("#auth-login-password").value = "";
  $("#auth-error").textContent = "";
  window.setTimeout(() => $("#auth-login-username").focus(), 0);
}

function hideAuthGate() {
  $("#auth-gate").hidden = true;
  $("#auth-gate").setAttribute("aria-hidden", "true");
}

async function loadAuthStatus() {
  const response = await fetch("/api/auth/status");
  const auth = await response.json();
  state.auth = auth;
  if (auth.enabled && !auth.authenticated) {
    showAuthGate(auth);
    return false;
  }
  hideAuthGate();
  return true;
}

async function login(event) {
  event.preventDefault();
  const error = $("#auth-error");
  error.textContent = "登录中…";
  try {
    const data = await postJson("/api/auth/login", { username: $("#auth-login-username").value.trim(), password: $("#auth-login-password").value });
    state.auth = { ...state.auth, enabled: true, authenticated: true, username: data.username || "" };
    hideAuthGate();
    await loadLibrary();
  } catch (loginError) {
    error.textContent = loginError.message;
  }
}

async function logout() {
  try {
    await postJson("/api/auth/logout");
    state.auth = { ...state.auth, enabled: true, authenticated: false, username: "" };
    showAuthGate(state.auth);
  } catch (error) {
    showToast(`退出失败：${error.message}`);
  }
}

async function selectSource(path = null) {
  try {
    await postJson("/api/source/select", path ? { path } : {});
    closeSettings();
    showToast("图库目录已更新，正在重新扫描…");
    await loadLibrary();
  } catch (error) {
    showToast(`目录授权失败：${error.message}`);
  }
}

async function selectDestination() {
  try {
    const data = await postJson("/api/destination/select");
    state.settings = data;
    renderSettings(state.settings, null);
    showToast("导出目录已更新");
  } catch (error) {
    showToast(`导出目录未更新：${error.message}`);
  }
}

async function selectEditedSource() {
  try {
    const data = await postJson("/api/edited/select");
    state.settings = data;
    renderSettings(state.settings, null);
    showToast("精选成片目录已更新，正在扫描…");
    await loadLibrary();
  } catch (error) {
    showToast(`精选成片目录未更新：${error.message}`);
  }
}

async function saveSettings() {
  try {
    const data = await postJson("/api/settings", { libraryPath: $("#source-path").value.trim(), seestarSource: $("#seestar-source").value.trim(), destination: $("#destination-path").value.trim(), editedSource: $("#edited-path").value.trim(), exportScope: $("#export-scope").value, galleryMode: $("#gallery-mode").value, mergeTargets: $("#merge-targets").checked, port: $("#server-port").value, r2: collectR2Config() });
    state.settings = data;
    closeSettings();
    showToast("设置已保存；导出范围立即生效，端口将在下次启动时生效");
    await loadLibrary();
  } catch (error) {
    showToast(`设置保存失败：${error.message}`);
  }
}

async function updateTags(item, tags) {
  try {
    await postJson("/api/tags", { target: item.category, tags });
    await loadLibrary();
    openDetails(state.lookup.get(item.id));
    showToast("标签已更新");
  } catch (error) {
    showToast(`标签保存失败：${error.message}`);
  }
}

async function openFolder(relative) {
  try {
    await postJson("/api/open-folder", { path: relative });
    showToast("已在访达中打开 SUB 文件夹");
  } catch (error) {
    showToast(`打开文件夹失败：${error.message}`);
  }
}

async function loadSubCollection(target) {
  const response = await fetch(`/api/sub-collection?target=${encodeURIComponent(target)}`);
  const data = await response.json();
  if (response.status === 401) showAuthGate(state.auth);
  if (!response.ok || data.ok === false) throw new Error(data.error || "无法加载 SUB 集合");
  const collection = { ...data.collection, loaded: true };
  state.subCollections[target] = collection;
  collection.groups.forEach((group) => group.items.forEach((item) => state.lookup.set(item.id, item)));
  return collection;
}

async function openDetails(item) {
  if (!item) return;
  if (!$("#details-panel").classList.contains("is-open")) state.detailsReturnFocus = document.activeElement;
  const request = ++state.detailsRequest;
  $("#details-panel").inert = false;
  const master = item;
  state.detailsItem = item;
  let collection = state.subCollections[master.category];
  if (collection && !collection.loaded) {
    $("#details-title").textContent = master.category;
    $("#details-content").innerHTML = `<div class="loading-state"><span class="loader"></span><p>正在加载 SUB 集合…</p></div>`;
    $("#details-panel").classList.add("is-open");
    $("#details-backdrop").classList.add("is-open");
    $("#details-panel").setAttribute("aria-hidden", "false");
    $("#details-backdrop").setAttribute("aria-hidden", "false");
    $("#close-details").focus();
    try {
      collection = await loadSubCollection(master.category);
      if (request !== state.detailsRequest) return;
    } catch (error) {
      if (request !== state.detailsRequest) return;
      $("#details-content").innerHTML = `<div class="details-empty">${escapeHtml(error.message)}</div>`;
      return;
    }
  }
  const groups = collection?.groups || [];
  const edited = state.editedByTarget[master.category] || [];
  const tags = master.tags || [];
  const masters = state.items.filter((candidate) => ["photo", "raw"].includes(candidate.kind) && candidate.category === master.category).sort((a, b) => imagePriority(a) - imagePriority(b) || b.capturedAt.localeCompare(a.capturedAt));
  const masterResults = masters.length > 1 ? `<div class="details-section-title"><span>单张成片</span><b>${masters.length} 张</b></div><div class="master-results">${masters.map((version) => `<button class="master-result" data-master-id="${escapeHtml(version.id)}"><img src="${escapeHtml(previewUrl(version))}" alt="${escapeHtml(version.name)}" /><span>${escapeHtml(version.captureDate || version.name)}</span></button>`).join("")}</div>` : "";
  const remoteMapping = master.remoteUri ? `<section class="remote-mapping"><div class="details-section-title"><span>Cloudflare R2</span><b>已同步</b></div><code>${escapeHtml(master.remoteUri)}</code>${master.publicUri ? `<a href="${escapeHtml(master.publicUri)}" target="_blank" rel="noreferrer">打开公开预览</a>` : `<small>尚未配置公开访问地址</small>`}</section>` : "";
  $("#details-title").textContent = master.category;
  $("#details-content").innerHTML = `<div class="details-master"><img src="${escapeHtml(previewUrl(master))}" alt="${escapeHtml(master.name)}" /><div><strong>${escapeHtml(master.name)}</strong><p>${escapeHtml(master.captureDate || "未知日期")} · ${escapeHtml(master.device || "未知设备")}</p><span>${formatBytes(master.size)} · ${escapeHtml(master.extension.toUpperCase())}</span></div></div>
    <section class="target-delete"><button id="delete-target" class="button danger" type="button">删除整个目标…</button><small>删除该目标在图库中的全部拍摄图和 SUB</small></section>
    <section class="photo-rating-editor"><div class="details-section-title"><span>照片评分</span><b>仅当前照片</b></div><div id="detail-rating" data-item-id="${escapeHtml(master.id)}" data-scope="${escapeHtml(master.ratingScope || "")}">${ratingEditor(master)}</div></section>
    <section class="workflow-editor"><div class="details-section-title"><span>目标工作流</span><b>同一目标共享</b></div><div class="workflow-controls"><label for="workflow-stage">处理阶段</label><select id="workflow-stage">${Object.entries(stageLabels).map(([key,label]) => `<option value="${key}" ${workflow(master).stage === key ? "selected" : ""}>${label}</option>`).join("")}</select><button id="detail-favorite" class="button secondary">${workflow(master).favorite ? "★ 已收藏" : "☆ 收藏"}</button></div><label class="notes-label" for="workflow-notes">后期备注</label><textarea id="workflow-notes" rows="3" maxlength="4000" placeholder="记录拉伸、去梯度参数，或下一步处理计划…">${escapeHtml(workflow(master).notes)}</textarea><button id="save-workflow" class="button primary">保存工作流</button><div class="capture-facts"><span>曝光 <b>${escapeHtml(master.exposure || "—")}${master.exposure ? " s" : ""}</b></span><span>滤镜 <b>${escapeHtml(master.filter || "—")}</b></span></div></section>
    ${master.kind === "raw" ? `<p class="settings-help">FIT 自适应预览（最长边 960 px）。打开大图可切换原样显示、调节黑场和亮度；仅影响预览。</p><a class="button secondary" href="${escapeHtml(master.originalUrl)}" download>下载原始 FIT</a>` : ""}
    ${masterResults}${remoteMapping}
    <section class="tag-editor"><div class="details-section-title"><span>标签 / 分组</span><b>自定义</b></div><div id="detail-tags" class="detail-tags">${tags.length ? tags.map((tag) => `<button class="tag-chip detail-tag" data-remove-tag="${escapeHtml(tag)}">${escapeHtml(tag)} ×</button>`).join("") : `<small>还没有标签</small>`}</div><div class="tag-input-row"><input id="new-tag" type="text" placeholder="添加标签，例如 最爱、待发布" /><button id="add-tag" class="button secondary">添加</button></div></section>
    <div class="details-section-title"><span>精选成片</span><b>${edited.length} 个版本</b></div>${edited.length ? `<div class="edited-results">${edited.map((version) => `<button class="edited-result" data-edited-id="${escapeHtml(version.id)}"><img src="${escapeHtml(previewUrl(version))}" alt="${escapeHtml(version.name)}" /><span>${escapeHtml(version.name)}</span></button>`).join("")}</div>` : `<div class="details-empty">尚未导入这个目标的精选成片。</div>`}
    <div class="details-section-title"><span>SUB 拍摄集合</span><b>${collection ? `${collection.photos} JPG · ${collection.raw} FIT` : "0 个文件"}</b></div>
    ${groups.length ? groups.map((group) => {
      const photoItems = [...group.items].sort((a, b) => imagePriority(a) - imagePriority(b) || b.capturedAt.localeCompare(a.capturedAt));
      const previewItems = photoItems.slice(0, 9);
      const remaining = Math.max(photoItems.length - previewItems.length, 0);
      return `<section class="sub-group"><div class="sub-group-head"><div><strong>${escapeHtml(group.date)}</strong><span>${escapeHtml(group.device)}</span></div><b>${group.photos} JPG · ${group.raw} FIT</b></div><div class="sub-group-meta"><span>${group.filters.length ? `滤镜 ${escapeHtml(group.filters.join(" / "))}` : "无滤镜信息"}</span><button class="sub-open-folder" data-open-folder="${escapeHtml(group.folder || "")}" type="button">在访达中打开</button></div><div class="sub-thumbs">${previewItems.map((subItem) => `<button class="sub-thumb" data-sub-id="${escapeHtml(subItem.id)}" title="预览 ${escapeHtml(subItem.name)}"><img src="${escapeHtml(previewUrl(subItem))}" loading="lazy" alt="${escapeHtml(subItem.name)}" /></button>`).join("")}</div>${remaining ? `<small class="sub-more">还有 ${remaining} 张图像，完整文件仍保留在本地目录</small>` : ""}</section>`;
    }).join("") : `<div class="details-empty">这个目标没有关联的 SUB 集合。</div>`}`;
  document.querySelectorAll(".sub-thumb").forEach((button) => button.addEventListener("click", () => openLightbox(state.lookup.get(button.dataset.subId))));
  document.querySelectorAll("[data-open-folder]").forEach((button) => button.addEventListener("click", (event) => { event.stopPropagation(); openFolder(button.dataset.openFolder); }));
  document.querySelectorAll(".edited-result").forEach((button) => button.addEventListener("click", () => openLightbox(state.lookup.get(button.dataset.editedId))));
  document.querySelectorAll(".master-result").forEach((button) => button.addEventListener("click", () => openLightbox(state.lookup.get(button.dataset.masterId))));
  document.querySelectorAll("[data-remove-tag]").forEach((button) => button.addEventListener("click", () => updateTags(master, tags.filter((tag) => tag !== button.dataset.removeTag))));
  $("#delete-target").addEventListener("click", () => previewTargetCleanup(master.category));
  $("#detail-rating").addEventListener("click", (event) => { const button = event.target.closest("[data-rating-value]"); if (button) saveRating(master, Number(button.dataset.ratingValue)); });
  $("#detail-favorite").addEventListener("click", async () => {
    if (await saveWorkflow(master, { favorite: !workflow(master).favorite })) $("#detail-favorite").textContent = workflow(master).favorite ? "★ 已收藏" : "☆ 收藏";
  });
  $("#save-workflow").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    await saveWorkflow(master, { stage: $("#workflow-stage").value, notes: $("#workflow-notes").value });
    button.disabled = false;
  });
  $("#add-tag").addEventListener("click", () => {
    const value = $("#new-tag").value.trim();
    if (value) updateTags(master, [...tags, value]);
  });
  $("#details-panel").classList.add("is-open");
  $("#details-backdrop").classList.add("is-open");
  $("#details-panel").setAttribute("aria-hidden", "false");
  $("#details-backdrop").setAttribute("aria-hidden", "false");
  $("#close-details").focus();
}

function closeDetails() {
  const wasOpen = $("#details-panel").classList.contains("is-open");
  state.detailsRequest++;
  $("#details-panel").inert = true;
  $("#details-panel").classList.remove("is-open");
  $("#details-backdrop").classList.remove("is-open");
  $("#details-panel").setAttribute("aria-hidden", "true");
  $("#details-backdrop").setAttribute("aria-hidden", "true");
  if (wasOpen) (state.detailsReturnFocus?.isConnected ? state.detailsReturnFocus : $("#search")).focus();
}

function openLightbox(item) {
  if (!item) return;
  if (!$("#lightbox").classList.contains("is-open")) {
    state.lightboxReturnFocus = document.activeElement;
    const visible = sortedItems(visibleItems());
    state.lightboxItems = visible.some((entry) => entry.id === item.id) ? visible : [...state.lookup.values()].filter((entry) => ["photo", "raw"].includes(entry.kind) && entry.category === item.category && Boolean(entry.isSub) === Boolean(item.isSub));
  }
  state.lightboxItem = item;
  $("#lightbox").classList.remove("is-zoomed");
  $("#lightbox-zoom").textContent = "放大";
  $("#lightbox-zoom").setAttribute("aria-pressed", "false");
  updateLightboxControls();
  $("#lightbox-image").hidden = false;
  $("#lightbox-image").parentElement.querySelector(".preview-error")?.remove();
  $("#lightbox-image").src = previewUrl(item);
  syncFitsControls(item);
  $("#lightbox-image").alt = item.name;
  $("#lightbox-title").textContent = item.name;
  $("#lightbox-meta").textContent = `${item.category} · ${item.captureDate || item.modified.replace("T", " ")} · ${item.device || "未知设备"}`;
  $("#lightbox").classList.add("is-open");
  $("#lightbox").setAttribute("aria-hidden", "false");
  $("#close-lightbox").focus();
}

function updateLightboxControls() {
  const index = state.lightboxItems.findIndex((item) => item.id === state.lightboxItem.id);
  $("#lightbox-position").textContent = `${index + 1} / ${state.lightboxItems.length}`;
  $("#lightbox-prev").disabled = index <= 0;
  $("#lightbox-next").disabled = index >= state.lightboxItems.length - 1;
  $("#lightbox-rating").innerHTML = ratingEditor(state.lightboxItem);
  $("#lightbox-favorite").textContent = workflow(state.lightboxItem).favorite ? "★ 已收藏" : "☆ 收藏";
}
function stepLightbox(direction) {
  const index = state.lightboxItems.findIndex((item) => item.id === state.lightboxItem.id);
  const next = state.lightboxItems[index + direction];
  if (next) openLightbox(next);
}
function showToast(message) {
  const toast = $("#toast");
  toast.textContent = message;
  toast.classList.add("is-visible");
  window.clearTimeout(showToast.timer);
  showToast.timer = window.setTimeout(() => toast.classList.remove("is-visible"), 3200);
}

function setViewMode(mode) {
  state.viewMode = mode === "list" ? "list" : "grid";
  $("#grid-view").classList.toggle("is-active", state.viewMode === "grid");
  $("#list-view").classList.toggle("is-active", state.viewMode === "list");
  $("#grid-view").setAttribute("aria-pressed", String(state.viewMode === "grid"));
  $("#list-view").setAttribute("aria-pressed", String(state.viewMode === "list"));
  renderGallery();
}

function setMobileNav(open) {
  const expanded = Boolean(open);
  $("#sidebar").classList.toggle("is-open", expanded);
  $("#mobile-nav-backdrop").classList.toggle("is-open", expanded);
  $("#mobile-nav-backdrop").setAttribute("aria-hidden", String(!expanded));
  $("#mobile-menu").setAttribute("aria-expanded", String(expanded));
  $("#mobile-menu").setAttribute("aria-label", expanded ? "关闭照片库导航" : "打开照片库导航");
}

function setFiltersExpanded(open) {
  const expanded = Boolean(open);
  $(".filters-bar").classList.toggle("is-expanded", expanded);
  $("#toggle-filters").setAttribute("aria-expanded", String(expanded));
  $("#toggle-filters").textContent = expanded ? "收起筛选" : "筛选";
}

function closeLightbox() {
  const wasOpen = $("#lightbox").classList.contains("is-open");
  $("#lightbox").classList.remove("is-open");
  $("#lightbox").setAttribute("aria-hidden", "true");
  if (wasOpen) (state.lightboxReturnFocus?.isConnected ? state.lightboxReturnFocus : $("#search")).focus();
}

let targetCleanupPlan = null;
let targetCleanupRunning = false;

function renderTargetCleanup(status) {
  $("#target-cleanup-summary").textContent = status.phase === "scanning"
    ? `正在统计…已找到 ${status.count} 个文件`
    : status.phase === "ready"
      ? `拍摄图 ${status.masters} 个，SUB ${status.subs} 个，共 ${status.count} 个文件（${formatBytes(status.bytes)}）。`
      : `${status.running ? "正在移入废纸篓" : status.phase === "completed" ? "删除完成" : status.phase === "failed" ? "操作失败" : "操作已停止"}：已移入 ${status.moved} 个，跳过 ${status.skipped} 个，失败 ${status.failed} 个。${status.error ? ` ${status.error}` : ""}`;
  $("#confirm-target-cleanup").disabled = status.running || status.phase !== "ready" || status.count === 0;
  $("#cancel-target-cleanup").textContent = status.running ? "停止" : status.phase === "ready" ? "取消" : "关闭";
}

async function pollTargetCleanup(status) {
  while (status.running) {
    renderTargetCleanup(status);
    await new Promise((resolve) => setTimeout(resolve, 750));
    const response = await fetch("/api/cleanup-target");
    const next = await response.json();
    if (!response.ok || next.ok === false) throw new Error(next.error || "无法获取删除进度");
    if (next.jobId !== status.jobId) throw new Error("清理任务已变化，请关闭后重新统计");
    status = next;
  }
  renderTargetCleanup(status);
  return status;
}

async function previewTargetCleanup(target) {
  if (targetCleanupRunning) return;
  targetCleanupRunning = true;
  targetCleanupPlan = { target, source: state.settings?.libraryPath || state.settings?.source, mergeTargets: state.mergeTargets };
  $("#target-cleanup-title").textContent = `删除整个目标：${target}`;
  $("#target-cleanup-source").textContent = targetCleanupPlan.source;
  $("#target-cleanup-summary").textContent = "正在统计拍摄图和 SUB…";
  $("#confirm-target-cleanup").disabled = true;
  $("#cancel-target-cleanup").disabled = true;
  $("#target-cleanup-dialog").showModal();
  try {
    const status = await postJson("/api/cleanup-target/preview", targetCleanupPlan);
    targetCleanupPlan.jobId = status.jobId;
    $("#cancel-target-cleanup").disabled = false;
    const result = await pollTargetCleanup(status);
    targetCleanupPlan.token = result.token;
  } catch (error) {
    $("#target-cleanup-summary").textContent = `无法统计：${error.message}`;
  } finally {
    targetCleanupRunning = false;
    $("#cancel-target-cleanup").disabled = false;
    $("#cancel-target-cleanup").textContent = targetCleanupPlan?.token ? "取消" : "关闭";
  }
}

async function confirmTargetCleanup() {
  if (!targetCleanupPlan?.token || targetCleanupRunning) return;
  targetCleanupRunning = true;
  $("#confirm-target-cleanup").disabled = true;
  $("#cancel-target-cleanup").disabled = true;
  $("#target-cleanup-summary").textContent = "正在提交删除任务…";
  try {
    const status = await postJson("/api/cleanup-target/execute", targetCleanupPlan);
    targetCleanupPlan.token = "";
    $("#cancel-target-cleanup").disabled = false;
    await pollTargetCleanup(status);
  } catch (error) {
    $("#target-cleanup-summary").textContent = `无法确认删除结果：${error.message}。任务可能仍在后台处理，请稍后重新统计；已移入废纸篓的文件不会自动恢复。`;
  } finally {
    targetCleanupPlan = null;
    targetCleanupRunning = false;
    $("#confirm-target-cleanup").disabled = true;
    $("#cancel-target-cleanup").disabled = false;
    $("#cancel-target-cleanup").textContent = "关闭";
    closeLightbox();
    closeDetails();
    await loadLibrary(true);
  }
}

async function cancelTargetCleanup() {
  if (!targetCleanupRunning) { $("#target-cleanup-dialog").close(); return; }
  const button = $("#cancel-target-cleanup");
  button.disabled = true;
  try {
    await postJson("/api/cleanup-target/cancel", { jobId: targetCleanupPlan?.jobId });
    button.textContent = "正在停止…";
  } catch (error) { showToast(error.message); button.disabled = false; }
}

let cleanupPlan = null;
let cleanupRunning = false;

async function previewJpegCleanup() {
  const button = $("#cleanup-jpg");
  button.disabled = true;
  button.textContent = "统计 JPG…";
  try {
    cleanupPlan = await postJson("/api/cleanup-jpg/preview");
    $("#cleanup-source").textContent = cleanupPlan.source;
    $("#cleanup-summary").textContent = `共 ${cleanupPlan.count} 个 JPG / JPEG，${formatBytes(cleanupPlan.bytes)}。`;
    $("#confirm-cleanup").disabled = cleanupPlan.count === 0;
    $("#confirm-cleanup").textContent = "移入废纸篓";
    $("#cancel-cleanup").textContent = "取消";
    $("#cleanup-dialog").showModal();
  } catch (error) {
    showToast(`无法统计 JPG：${error.message}`);
  } finally {
    button.disabled = false;
    button.textContent = "清理 JPG";
  }
}

async function confirmJpegCleanup() {
  if (!cleanupPlan || cleanupRunning) return;
  cleanupRunning = true;
  $("#confirm-cleanup").disabled = true;
  $("#cancel-cleanup").disabled = true;
  $("#confirm-cleanup").textContent = "正在移入废纸篓…";
  try {
    const result = await postJson("/api/cleanup-jpg", { token: cleanupPlan.token });
    const summary = `已移入废纸篓 ${result.moved} 个，跳过 ${result.skipped} 个，失败 ${result.failed} 个。`;
    $("#cleanup-summary").textContent = summary + (result.errors.length ? ` ${result.errors.slice(0, 3).map((item) => `${item.path}：${item.error}`).join("；")}` : "");
    showToast(summary);
  } catch (error) {
    $("#cleanup-summary").textContent = `清理未完成：${error.message}。请关闭后重新统计目录。`;
  } finally {
    cleanupPlan = null;
    cleanupRunning = false;
    $("#confirm-cleanup").textContent = "移入废纸篓";
    $("#cancel-cleanup").disabled = false;
    $("#cancel-cleanup").textContent = "关闭";
    closeLightbox();
    closeDetails();
    await loadLibrary(true);
  }
}

async function detectSeestar() {
  $("#detect-seestar").disabled = true;
  try {
    const data = await postJson("/api/seestar/detect");
    renderSeestarCandidates(data.candidates || []);
  } catch (error) { showToast(error.message); }
  finally { $("#detect-seestar").disabled = false; }
}

async function importSeestar() {
  const button = $("#import-seestar");
  button.disabled = true;
  try {
    await postJson("/api/settings", { seestarSource: $("#seestar-source").value.trim() });
    await postJson("/api/seestar/import");
    while (true) {
      const response = await fetch("/api/seestar/import");
      const status = await response.json();
      if (!response.ok) throw new Error(status.error || "无法读取导入进度");
      $("#seestar-import-status").textContent = `${status.running ? "导入中" : "导入结束"}：新增 ${status.imported} 个，同名跳过 ${status.skipped} 个${status.current ? ` · ${status.current}` : ""}${status.error ? ` · ${status.error}` : ""}`;
      if (!status.running) { await loadLibrary(true); break; }
      await new Promise((resolve) => setTimeout(resolve, 1000));
    }
  } catch (error) { showToast(`导入失败：${error.message}`); }
  finally { button.disabled = false; }
}

async function exportFiles(mode) {
  const button = mode === "jpg" ? $("#export-jpg") : $("#export-all");
  const original = button.textContent;
  button.disabled = true;
  button.textContent = "导出中…";
  try {
    const payload = await postJson("/api/export", { mode });
    const summary = payload.output.split("\n").find((line) => line.includes("导出完成")) || "导出完成";
    showToast(summary);
  } catch (error) {
    showToast(`导出失败：${error.message}`);
  } finally {
    button.disabled = false;
    button.textContent = original;
  }
}

async function loadLibrary(forceRefresh = false) {
  $("#refresh").disabled = true;
  $("#refresh").classList.add("is-loading");
  $("#library-status").innerHTML = '<span class="loader mini-loader"></span><span>扫描中…</span>';
  try {
    if (!(await loadAuthStatus())) return;
    const response = await fetch("/api/library" + (forceRefresh ? "?refresh=1" : ""));
    const library = await response.json();
    if (!response.ok) throw new Error(library.error || "无法读取媒体库");
    state.workflows = library.targetWorkflow || {};
    state.items = library.items || [];
    state.subCollections = library.subCollections || {};
    state.editedItems = library.editedItems || [];
    state.sourceStatus = library.sourceStatus || {};
    state.editedStatus = library.editedStatus || {};
    state.editedByTarget = library.editedByTarget || {};
    state.facets = library.facets || { targets: [], devices: [], dates: [], tags: [] };
    state.settings = library.settings || state.settings;
    state.stats = library.stats || {};
    state.scan = library.scan || state.scan;
    state.galleryMode = state.settings?.galleryMode || "auto";
    state.mergeTargets = state.settings?.mergeTargets !== false;
    state.lookup = new Map(state.items.map((item) => [item.id, item]));
    state.editedItems.forEach((item) => state.lookup.set(item.id, item));
    Object.values(state.subCollections).forEach((collection) => collection.groups.forEach((group) => (group.items || []).forEach((item) => state.lookup.set(item.id, item))));
    renderAccess(library);
    renderScan(state.scan);
    renderSidebar();
    renderFilters(state.facets);
    renderSettings(state.settings, library.sourceStatus);
    renderHero(state.stats);
    updateSectionTitle();
    renderGallery();
  } catch (error) {
    $("#library-status").innerHTML = '<span class="status-dot status-dot-error"></span><span>扫描失败</span>';
   $("#gallery").innerHTML = `<div class="empty-state"><div><strong>无法读取本地作品库</strong><p>${escapeHtml(error.message)}</p></div></div>`;
  } finally {
    $("#refresh").disabled = false;
    $("#refresh").classList.remove("is-loading");
  }
}

$("#search").addEventListener("input", (event) => { state.query = event.target.value; renderGallery(); });
$("#confirm-target-cleanup").addEventListener("click", confirmTargetCleanup);
$("#cancel-target-cleanup").addEventListener("click", cancelTargetCleanup);
$("#target-cleanup-dialog").addEventListener("cancel", (event) => { if (targetCleanupRunning) event.preventDefault(); });
$("#mobile-menu").addEventListener("click", () => setMobileNav(!$("#sidebar").classList.contains("is-open")));
$("#mobile-nav-backdrop").addEventListener("click", () => setMobileNav(false));
$("#toggle-filters").addEventListener("click", () => setFiltersExpanded(!$(".filters-bar").classList.contains("is-expanded")));
$("#refresh").addEventListener("click", () => loadLibrary(true));
$("#settings").addEventListener("click", openSettings);
$("#cleanup-jpg").addEventListener("click", previewJpegCleanup);
$("#confirm-cleanup").addEventListener("click", confirmJpegCleanup);
$("#cancel-cleanup").addEventListener("click", () => { cleanupPlan = null; $("#cleanup-dialog").close(); });
$("#cleanup-dialog").addEventListener("cancel", (event) => { if (cleanupRunning) event.preventDefault(); else cleanupPlan = null; });
// Capturing handles lazy-loaded failures in cards, details, SUB and the lightbox.
document.addEventListener("error", (event) => {
  const image = event.target;
  if (image.tagName !== "IMG" || !image.getAttribute("src")?.startsWith("/api/fits-preview/")) return;
  image.hidden = true;
  if (!image.parentElement.querySelector(".preview-error")) {
    const message = document.createElement("span");
    message.className = "preview-error";
    message.textContent = "FIT 预览失败：文件损坏或格式暂不支持";
    image.parentElement.append(message);
  }
}, true);
$("#export-jpg").addEventListener("click", () => exportFiles("jpg"));
$("#export-all").addEventListener("click", () => exportFiles("all"));
$("#gallery-mode").addEventListener("change", async (event) => {
  state.galleryMode = event.target.value;
  renderFilters(state.facets);
  renderSidebar();
  renderHero(state.stats);
  renderGallery();
  try {
    state.settings = await postJson("/api/settings", { galleryMode: state.galleryMode });
  } catch (error) {
    showToast(`展示模式保存失败：${error.message}`);
  }
});
$("#merge-targets").addEventListener("change", async (event) => {
  const next = event.target.checked;
  try {
    const data = await postJson("/api/settings", { mergeTargets: next });
    state.settings = data;
    state.mergeTargets = data.mergeTargets !== false;
    await loadLibrary();
    showToast(state.mergeTargets ? "已合并重复目标" : "已按原始目标文件夹展示");
  } catch (error) {
    event.target.checked = state.mergeTargets;
    showToast("目标合并设置失败：" + error.message);
  }
});
$("#target-filter").addEventListener("change", (event) => { state.filters.target = event.target.value; renderSidebar(); updateSectionTitle(); renderGallery(); });
$("#device-filter").addEventListener("change", (event) => { state.filters.device = event.target.value; renderGallery(); });
$("#rating-filter").addEventListener("change", (event) => { state.filters.rating = event.target.value; renderGallery(); });
$("#tag-filter").addEventListener("change", (event) => { state.filters.tag = event.target.value; renderGallery(); });
$("#date-from").addEventListener("change", (event) => { state.filters.dateFrom = event.target.value; renderGallery(); });
$("#date-to").addEventListener("change", (event) => { state.filters.dateTo = event.target.value; renderGallery(); });
$("#sort-filter").addEventListener("change", (event) => { state.filters.sort = event.target.value; renderGallery(); });
$("#grid-view").addEventListener("click", () => setViewMode("grid"));
$("#list-view").addEventListener("click", () => setViewMode("list"));
$("#clear-filters").addEventListener("click", resetFilters);
$("#pick-source-banner").addEventListener("click", () => state.galleryMode === "edited" ? selectEditedSource() : selectSource());
$("#retry-source").addEventListener("click", () => loadLibrary(true));
$("#detect-seestar").addEventListener("click", detectSeestar);
$("#import-seestar").addEventListener("click", importSeestar);
$("#pick-source").addEventListener("click", () => selectSource());
$("#pick-destination").addEventListener("click", selectDestination);
$("#pick-edited").addEventListener("click", selectEditedSource);
$("#save-settings").addEventListener("click", saveSettings);
$("#test-r2").addEventListener("click", testR2Connection);
$("#sync-r2").addEventListener("click", syncR2Now);
$("#r2-enabled").addEventListener("change", (event) => { if (!event.target.checked) $("#r2-auto-upload").checked = false; });
$("#r2-auto-upload").addEventListener("change", (event) => { if (event.target.checked) $("#r2-enabled").checked = true; });
$("#auth-form").addEventListener("submit", login);
$("#logout").addEventListener("click", logout);
$("#sidebar").addEventListener("click", (event) => {
  const button = event.target.closest(".nav-item");
  if (!button) return;
  state.workspace = button.dataset.workspace || "all";
  state.filters.target = button.dataset.target || "all";
  $("#target-filter").value = state.filters.target;
  renderSidebar(); updateSectionTitle(); renderGallery(); setMobileNav(false);
});
$("#sidebar-settings").addEventListener("click", () => { setMobileNav(false); openSettings(); });
document.querySelectorAll("[data-mode]").forEach((button) => button.addEventListener("click", () => {
  $("#gallery-mode").value = button.dataset.mode;
  $("#gallery-mode").dispatchEvent(new Event("change"));
}));
$("#active-filters").addEventListener("click", (event) => { if (event.target.closest("[data-reset]")) resetFilters(); });
$("#gallery").addEventListener("click", async (event) => {
  if (event.target.closest("[data-reset]")) return resetFilters();
  if (event.target.closest("[data-settings]")) return openSettings();
  const favorite = event.target.closest("[data-favorite-id]");
  if (favorite) { favorite.disabled = true; const item = state.lookup.get(favorite.dataset.favoriteId); await saveWorkflow(item, {favorite: !workflow(item).favorite}); favorite.disabled = false; return; }
  const info = event.target.closest("[data-info-id]");
  if (info) return openDetails(state.lookup.get(info.dataset.infoId));
  const preview = event.target.closest("[data-preview-id]");
  if (preview) openLightbox(state.lookup.get(preview.dataset.previewId));
});
["#fits-mode", "#fits-black", "#fits-brightness", "#fits-neutralize"].forEach((selector) => $(selector).addEventListener("change", updateFitsPreview));
["#fits-black", "#fits-brightness"].forEach((selector) => $(selector).addEventListener("input", (event) => {
  $(`${selector}-value`).textContent = Number(event.target.value).toFixed(1);
}));
$("#fits-reset").addEventListener("click", () => {
  if (!state.lightboxItem) return;
  state.fitsOptions.delete(fitsOptionKey(state.lightboxItem));
  syncFitsControls(state.lightboxItem);
  updateFitsPreview();
});
$("#lightbox-prev").addEventListener("click", () => stepLightbox(-1));
$("#lightbox-next").addEventListener("click", () => stepLightbox(1));
$("#lightbox-rating").addEventListener("click", (event) => { const button = event.target.closest("[data-rating-value]"); if (button) saveRating(state.lightboxItem, Number(button.dataset.ratingValue)); });
$("#lightbox-favorite").addEventListener("click", async (event) => {
  const button = event.currentTarget; button.disabled = true;
  await saveWorkflow(state.lightboxItem, {favorite: !workflow(state.lightboxItem).favorite}); button.disabled = false;
});
$("#lightbox-details").addEventListener("click", () => { closeLightbox(); openDetails(state.lightboxItem); });
$("#lightbox-zoom").addEventListener("click", () => {
  const zoomed = $("#lightbox").classList.toggle("is-zoomed");
  $("#lightbox-zoom").textContent = zoomed ? "适应屏幕" : "放大";
  $("#lightbox-zoom").setAttribute("aria-pressed", String(zoomed));
});
$("#close-lightbox").addEventListener("click", closeLightbox);
$("#lightbox").addEventListener("click", (event) => { if (event.target.id === "lightbox") closeLightbox(); });
$("#close-details").addEventListener("click", closeDetails);
$("#details-backdrop").addEventListener("click", closeDetails);
$("#close-settings").addEventListener("click", closeSettings);
$("#settings-backdrop").addEventListener("click", closeSettings);
document.addEventListener("keydown", (event) => {
  if (document.querySelector("dialog[open]")) return;
  const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(event.target.tagName);
  if (!typing && $("#lightbox").classList.contains("is-open")) {
    if (event.key === "ArrowLeft") { event.preventDefault(); stepLightbox(-1); }
    if (event.key === "ArrowRight") { event.preventDefault(); stepLightbox(1); }
    if (event.key.toLowerCase() === "f" && !event.repeat) $("#lightbox-favorite").click();
  }
  if (event.key === "Tab") {
    const panel = [$("#lightbox"), $("#settings-panel"), $("#details-panel")].find((entry) => entry.classList.contains("is-open"));
    if (panel) {
      const controls = [...panel.querySelectorAll('button:not(:disabled), input, select, textarea, a[href]')].filter((entry) => entry.getClientRects().length);
      const first = controls[0], last = controls[controls.length - 1];
      if (event.shiftKey && (document.activeElement === first || !panel.contains(document.activeElement))) { event.preventDefault(); last?.focus(); }
      else if (!event.shiftKey && (document.activeElement === last || !panel.contains(document.activeElement))) { event.preventDefault(); first?.focus(); }
    }
  }
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
    event.preventDefault();
    if (!document.querySelector(".lightbox.is-open, .details-panel.is-open")) {
      $("#search").focus();
      $("#search").select();
    }
  }
  if (event.key === "Escape") {
    if ($("#lightbox").classList.contains("is-open")) closeLightbox();
    else if ($("#settings-panel").classList.contains("is-open")) closeSettings();
    else if ($("#details-panel").classList.contains("is-open")) closeDetails();
    else setMobileNav(false);
  }
});
loadLibrary();
