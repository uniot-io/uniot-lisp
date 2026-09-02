// Resolve the .wasm from the site root rather than relative to the page that loaded this
// module. Without this, a page served from a nested path asks for the module beneath that
// path, where it does not exist.
//
// A host that serves the file from somewhere else sets globalThis.UNLISP_WASM_URL before
// loading this script. Browser only: under Node the default resolution -- next to this
// file -- is already right, and overriding it breaks the test harness.
if (typeof window !== "undefined") {
  Module["locateFile"] = function (path, scriptDirectory) {
    if (path.endsWith(".wasm")) {
      return (typeof globalThis !== "undefined" && globalThis.UNLISP_WASM_URL) || "/" + path;
    }
    return scriptDirectory + path;
  };
}
