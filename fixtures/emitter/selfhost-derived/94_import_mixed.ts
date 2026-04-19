// Mixed default + named import (single declaration), and a separate
// renamed default. Both forms appear in real-world code.

import React, { useState, useEffect as onEffect } from "react";
import defaultExport, * as ns from "./mod.js";

export function init(): void {
  defaultExport();
  ns.boot();
  React.createElement("div", null);
  useState(0);
  onEffect(() => {}, []);
}
