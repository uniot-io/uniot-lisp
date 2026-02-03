Module = Module || {};
Module["locateFile"] = function (path, scriptDirectory) {
  if (path.endsWith(".wasm")) {
    return "/unlisp.wasm";
  }
  return scriptDirectory + path;
};
