// ── Config Post API ───────────────────────────────────────────────────
// @web-module-requires: state, entity_catalog, entity_state, model_generated, api, config_codec, config_option_core

function saveButtonConfig(slot) {
  var b = state.buttons[slot - 1];
  postText(entityNameForSlot("button_config", slot), serializeButtonConfig(b));
}

function clearCardImageReferences(id) {
  id = normalizeCardBackgroundImageId(id);
  if (!id) return 0;
  var changed = 0;
  function clearButtons(buttons, save) {
    (buttons || []).forEach(function (button, index) {
      if (cardBackgroundImage(button && button.options) !== id) return;
      button.options = setConfigOptionValue(button.options, CARD_BACKGROUND_IMAGE_OPTION, "");
      changed++;
      if (save) save(index);
    });
  }
  clearButtons(state.buttons, function (index) { saveButtonConfig(index + 1); });
  Object.keys(state.subpages || {}).forEach(function (key) {
    var subpage = state.subpages[key];
    var before = changed;
    clearButtons(subpage && subpage.buttons);
    if (changed !== before) saveSubpageEntity(key);
  });
  return changed;
}

function subpageEntityKeys() {
  var keys = ENTITY_CATALOG.groups.subpage_slot || [];
  var count = (CFG.features && CFG.features.subpageConfigChunks) || keys.length;
  count = Math.max(1, Math.min(keys.length, parseInt(count, 10) || keys.length));
  return keys.slice(0, count);
}

var SUBPAGE_RAW_CHUNK_FIELDS = ["main", "ext", "ext2", "ext3", "ext4", "ext5", "ext6", "ext7"];

function subpageChunkShouldPost(slot, keys, chunks, index, previousPendingChunks) {
  if (chunks[index] || index === 0) return true;
  var chunkName = entityNameForSlot(keys[index], slot);
  if (hasRememberedPostPath("text", chunkName, [])) return true;
  var raw = state.subpageRaw[slot];
  var rawField = SUBPAGE_RAW_CHUNK_FIELDS[index];
  return !!(
    (raw && rawField && raw[rawField]) ||
    (previousPendingChunks && previousPendingChunks[index])
  );
}

function saveSubpageEntity(slot) {
  var sp = state.subpages[slot];
  var full = sp ? serializeSubpageConfig(sp) : "";
  var keys = subpageEntityKeys();
  var chunks = EspControlModel.splitSubpageConfigChunks(full, keys.length, 255);
  if (!chunks) {
    showBanner("Subpage is too large to save. Shorten labels or entity IDs.", "error");
    return;
  }
  var previousPendingChunks = EspControlModel.splitSubpageConfigChunks(
    state.subpageSavePending[slot] || "", keys.length, 255) || [];
  state.subpageSavePending[slot] = full;
  for (var ki = 0; ki < keys.length; ki++) {
    var chunkName = entityNameForSlot(keys[ki], slot);
    var chunk = chunks[ki] || "";
    if (!subpageChunkShouldPost(slot, keys, chunks, ki, previousPendingChunks)) continue;
    postText(chunkName, chunk);
  }
}

function scheduleSliderSubpageMigration(slot) {
  pendingSliderSubpageMigrations[slot] = true;
  clearTimeout(sliderMigrationTimer);
  sliderMigrationTimer = setTimeout(function () {
    var pending = pendingSliderSubpageMigrations;
    pendingSliderSubpageMigrations = {};
    for (var key in pending) {
      if (state.subpages[key]) saveSubpageEntity(key);
    }
  }, 5000);
}
