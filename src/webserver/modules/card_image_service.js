// ── Card image service ────────────────────────────────────────────────
// @web-module-requires: state, config_option_core

var _cardImageLibrary = [];
var _cardImageLibraryInfo = {
  available: false,
  requiresUsbFlash: false,
  formatVersion: 0,
  maxActiveBackgrounds: CARD_BACKGROUND_IMAGE_LIMIT,
  storageBytes: 0,
  usedBytes: 0,
  freeBytes: 0,
  maxBytes: 0
};
var CARD_IMAGE_TARGET_SIZE = 200;
var CARD_IMAGE_UPLOAD_MAX_BYTES = 45 * 1024;
var CARD_IMAGE_MIN_QUALITY = 0.42;

function listCardImages(force) {
  if (!force && _cardImageLibrary.length) return Promise.resolve(_cardImageLibrary.slice());
  return fetch("/api/card-images")
    .then(function (response) {
      if (!response.ok) throw new Error("Could not load images.");
      return response.json();
    })
    .then(function (data) {
      _cardImageLibrary = data && data.images ? data.images : [];
      _cardImageLibraryInfo = {
        available: !!(data && data.available),
        requiresUsbFlash: !!(data && data.requires_usb_flash),
        formatVersion: parseInt(data && data.format_version, 10) || 0,
        maxActiveBackgrounds: parseInt(data && data.max_active_backgrounds, 10) || CARD_BACKGROUND_IMAGE_LIMIT,
        storageBytes: parseInt(data && data.storage_bytes, 10) || 0,
        usedBytes: parseInt(data && data.used_bytes, 10) || 0,
        freeBytes: parseInt(data && data.free_bytes, 10) || 0,
        maxBytes: parseInt(data && data.max_bytes, 10) || 0
      };
      return _cardImageLibrary.slice();
    });
}

function cardImageLibraryInfo() {
  return Object.assign({}, _cardImageLibraryInfo);
}

function countCardImageUsage(id) {
  id = normalizeCardBackgroundImageId(id);
  if (!id) return 0;
  var count = 0;
  function countButtons(buttons) {
    (buttons || []).forEach(function (button) {
      if (cardBackgroundImage(button && button.options) === id) count++;
    });
  }
  countButtons(state.buttons);
  Object.keys(state.subpages || {}).forEach(function (key) {
    var subpage = state.subpages[key];
    countButtons(subpage && subpage.buttons);
  });
  return count;
}

function resizeCardImageFile(file) {
  return new Promise(function (resolve, reject) {
    if (!file || !file.type || file.type.indexOf("image/") !== 0) {
      reject(new Error("Choose an image file."));
      return;
    }
    var image = new Image();
    var objectUrl = URL.createObjectURL(file);
    image.onload = function () {
      URL.revokeObjectURL(objectUrl);
      var canvas = document.createElement("canvas");
      canvas.width = CARD_IMAGE_TARGET_SIZE;
      canvas.height = CARD_IMAGE_TARGET_SIZE;
      var context = canvas.getContext("2d");
      context.imageSmoothingEnabled = true;
      context.imageSmoothingQuality = "high";
      var scale = Math.max(CARD_IMAGE_TARGET_SIZE / image.naturalWidth, CARD_IMAGE_TARGET_SIZE / image.naturalHeight);
      var width = image.naturalWidth * scale;
      var height = image.naturalHeight * scale;
      context.drawImage(image, (CARD_IMAGE_TARGET_SIZE - width) / 2, (CARD_IMAGE_TARGET_SIZE - height) / 2, width, height);
      function finish(blob, quality) {
        if (!blob || blob.size > CARD_IMAGE_UPLOAD_MAX_BYTES) {
          reject(new Error("Image is still too large after browser optimization."));
          return;
        }
        blob.optimizedWidth = CARD_IMAGE_TARGET_SIZE;
        blob.optimizedHeight = CARD_IMAGE_TARGET_SIZE;
        blob.optimizedQuality = quality;
        resolve(blob);
      }
      function encode(quality) {
        if (canvas.toBlob) {
          canvas.toBlob(function (blob) {
            if (blob && blob.size > CARD_IMAGE_UPLOAD_MAX_BYTES && quality > CARD_IMAGE_MIN_QUALITY) {
              encode(Math.max(CARD_IMAGE_MIN_QUALITY, quality - 0.08));
            } else {
              finish(blob, quality);
            }
          }, "image/jpeg", quality);
          return;
        }
        var data = canvas.toDataURL("image/jpeg", quality);
        var raw = atob(data.substring(data.indexOf(",") + 1));
        var bytes = new Uint8Array(raw.length);
        for (var i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
        var blob = new Blob([bytes], { type: "image/jpeg" });
        if (blob.size > CARD_IMAGE_UPLOAD_MAX_BYTES && quality > CARD_IMAGE_MIN_QUALITY) {
          encode(Math.max(CARD_IMAGE_MIN_QUALITY, quality - 0.08));
        } else {
          finish(blob, quality);
        }
      }
      encode(0.78);
    };
    image.onerror = function () {
      URL.revokeObjectURL(objectUrl);
      reject(new Error("Could not read that image."));
    };
    image.src = objectUrl;
  });
}

function uploadCardImage(file) {
  return resizeCardImageFile(file).then(function (image) {
    return fetch("/api/card-images", {
      method: "POST",
      headers: { "Content-Type": "image/jpeg" },
      body: image,
    });
  })
    .then(function (response) {
      if (!response.ok) {
        return response.text().then(function (message) {
          throw new Error(message || "Could not upload image.");
        });
      }
      return response.json();
    })
    .then(function (item) {
      if (!item || !item.id) throw new Error("Could not upload image.");
      return item;
    });
}

function renameCardImage(id, name) {
  id = normalizeCardBackgroundImageId(id);
  if (!id) return Promise.reject(new Error("Could not rename image."));
  return fetch("/api/card-images/" + id + "/rename", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "name=" + encodeURIComponent(String(name || ""))
  })
    .then(function (response) {
      if (!response.ok) {
        return response.text().then(function (message) {
          throw new Error(message || "Could not rename image.");
        });
      }
      return response.json();
    });
}

function deleteCardImage(id) {
  id = normalizeCardBackgroundImageId(id);
  if (!id) return Promise.resolve();
  return fetch("/api/card-images/" + id, { method: "DELETE" })
    .then(function (response) {
      if (!response.ok) throw new Error("Could not delete image.");
    });
}
