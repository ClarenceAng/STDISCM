const REFRESH_MS = 1500;

const grid = document.getElementById("grid");
const empty = document.getElementById("empty");
const offline = document.getElementById("offline");
const activityList = document.getElementById("activityList");
const activityEmpty = document.getElementById("activityEmpty");
const dialog = document.getElementById("playerDialog");
const player = document.getElementById("player");
const playerTitle = document.getElementById("playerTitle");
const playerMessage = document.getElementById("playerMessage");
const queueFill = document.getElementById("queueFill");

// the codes of a browser MediaError
const MEDIA_ERRORS = {
  1: "loading was interrupted",
  2: "the network connection to the consumer failed",
  3: "the file is damaged or uses a format this browser can't decode",
  4: "the file is missing from storage or this browser can't play it",
};

// cards are only ever added, so a preview that is playing isn't interrupted by a refresh
const cards = new Set();

// a problem with the consumer's answer, as opposed to a bug in this page
class ServiceError extends Error {}

function formatBytes(bytes) {
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function formatDuration(seconds) {
  const total = Math.round(seconds);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function mediaErrorText(media) {
  return (media.error && MEDIA_ERRORS[media.error.code]) || "the browser gave no reason";
}

function showThumbNote(thumb, text) {
  thumb.querySelector(".thumb-note")?.remove();
  thumb.prepend(element("span", "thumb-note", text));
}

function startPreview(card, id) {
  const thumb = card.querySelector(".thumb");
  if (thumb.querySelector("video")) return;

  const preview = document.createElement("video");
  preview.muted = true;
  preview.loop = true;
  preview.playsInline = true;
  preview.autoplay = true;
  preview.addEventListener("error", () => {
    console.error(`preview of ${id} failed: ${mediaErrorText(preview)}`);
    preview.remove();
    showThumbNote(thumb, "Preview unavailable");
  });
  preview.src = `/media/${id}/preview`;
  thumb.insertBefore(preview, thumb.querySelector(".duration"));
}

function stopPreview(card) {
  const preview = card.querySelector(".thumb video");
  if (!preview) return;
  preview.pause();
  preview.removeAttribute("src");
  preview.load();
  preview.remove();
}

function showPlayerMessage(text) {
  playerMessage.textContent = text;
  playerMessage.hidden = false;
}

function openPlayer(video) {
  playerTitle.textContent = video.name;
  playerMessage.hidden = true;
  player.src = `/media/${video.id}/video`;
  if (!dialog.open) dialog.showModal();

  player.play().catch((error) => {
    if (error.name === "NotAllowedError") {
      showPlayerMessage("Your browser blocked autoplay. Press play to start the video.");
    } else if (error.name !== "AbortError") {  // AbortError is the viewer closing the player while it loads
      console.error(`could not start ${video.name}`, error);
      if (playerMessage.hidden) showPlayerMessage(`This video could not start: ${error.message}`);
    }
  });
}

function buildCard(video) {
  const card = element("button", "card");
  card.type = "button";
  card.dataset.id = video.id;
  card.dataset.savedAt = video.savedAtMs;

  const thumb = element("span", "thumb");
  const poster = document.createElement("img");
  poster.alt = "";
  poster.loading = "lazy";
  poster.addEventListener("error", () => {
    console.error(`poster of ${video.id} could not be loaded`);
    poster.remove();
    showThumbNote(thumb, "No thumbnail");
  });
  poster.src = `/media/${video.id}/poster`;
  thumb.append(poster, element("span", "duration", formatDuration(video.durationSeconds)));

  const body = element("span", "card-body");
  const name = element("span", "card-name", video.name);
  name.title = video.name;
  const sizes = element("span", "card-meta", `${formatBytes(video.originalBytes)} → ${formatBytes(video.savedBytes)}`);
  if (!video.hasAudio) sizes.append(element("span", "badge", "no audio"));
  body.append(name, element("span", "card-meta", video.producer), sizes);

  card.append(thumb, body);
  card.addEventListener("mouseenter", () => startPreview(card, video.id));
  card.addEventListener("mouseleave", () => stopPreview(card));
  card.addEventListener("focus", () => startPreview(card, video.id));
  card.addEventListener("blur", () => stopPreview(card));
  card.addEventListener("click", () => {
    stopPreview(card);
    openPlayer(video);
  });
  return card;
}

function renderVideos(videos) {
  const newest = [...videos].sort((a, b) => a.savedAtMs - b.savedAtMs);
  for (const video of newest) {
    if (cards.has(video.id)) continue;
    cards.add(video.id);
    grid.prepend(buildCard(video));
  }
  empty.hidden = cards.size > 0;
  document.getElementById("savedValue").textContent = cards.size;
}

function renderActivity(events) {
  activityEmpty.hidden = events.length > 0;
  activityList.replaceChildren(...events.map((event) => {
    const item = element("li");
    item.append(element("span", `event-kind event-${event.kind}`, event.kind), document.createTextNode(` ${event.file}`));

    const time = new Date(event.atMs).toLocaleTimeString();
    const parts = [time, event.producer];
    if (event.detail) parts.push(event.detail);
    item.append(element("span", "event-meta", parts.join(" · ")));
    return item;
  }));
}

function renderStatus(status) {
  document.getElementById("queueValue").textContent = `${status.queue.occupied} / ${status.queue.capacity}`;
  document.getElementById("workersValue").textContent = `${status.workers.busy} / ${status.workers.total}`;
  document.getElementById("droppedValue").textContent = status.totals.dropped;
  document.getElementById("duplicatesValue").textContent = status.totals.duplicates;
  document.getElementById("failedValue").textContent = status.totals.failed;

  queueFill.style.width = `${(100 * status.queue.occupied) / status.queue.capacity}%`;
  queueFill.classList.toggle("full", status.queue.occupied >= status.queue.capacity);
  renderActivity(status.recent);
}

async function getJson(url) {
  let response;
  try {
    response = await fetch(url, { cache: "no-store" });
  } catch (error) {
    throw new ServiceError("Can't reach the consumer. Retrying…", { cause: error });
  }

  if (!response.ok) {
    const reason = await response.text().catch((error) => `its explanation could not be read: ${error.message}`);
    throw new ServiceError(`The consumer reported a problem (${response.status}): ${reason || url}. Retrying…`);
  }

  try {
    return await response.json();
  } catch (error) {
    throw new ServiceError(`The consumer sent an answer this page can't read (${url}). Retrying…`, { cause: error });
  }
}

async function refresh() {
  try {
    const [status, videos] = await Promise.all([getJson("/api/status"), getJson("/api/videos")]);
    renderStatus(status);
    renderVideos(videos);
    offline.hidden = true;
  } catch (error) {
    console.error(error);
    offline.textContent = error instanceof ServiceError ? error.message : `This page couldn't show the latest data: ${error.message}. Retrying…`;
    offline.hidden = false;
  } finally {
    setTimeout(refresh, REFRESH_MS);
  }
}

document.getElementById("playerClose").addEventListener("click", () => dialog.close());
player.addEventListener("error", () => {
  if (!player.getAttribute("src")) return;  // the player was closed, which clears its source
  console.error(`player failed: ${mediaErrorText(player)}`);
  showPlayerMessage(`This video can't be played: ${mediaErrorText(player)}.`);
});
dialog.addEventListener("close", () => {
  player.pause();
  player.removeAttribute("src");
  player.load();
});

refresh();
