(function () {
  "use strict";

  const root = document.documentElement;
  const themeButton = document.querySelector("[data-theme-toggle]");
  const darkPreference = window.matchMedia("(prefers-color-scheme: dark)");

  function activeTheme() {
    return root.dataset.theme || (darkPreference.matches ? "dark" : "light");
  }

  function updateThemeButton() {
    if (!themeButton) return;
    const current = activeTheme();
    const next = current === "dark" ? "light" : "dark";
    const label = themeButton.querySelector("[data-theme-label]");
    const icon = themeButton.querySelector("[data-theme-icon]");
    if (label) label.textContent = next === "dark" ? "Dark" : "Light";
    if (icon) icon.textContent = next === "dark" ? "●" : "○";
    themeButton.setAttribute("aria-label", `Switch to ${next} theme`);
    themeButton.setAttribute("title", `Switch to ${next} theme`);
  }

  if (themeButton) {
    updateThemeButton();
    themeButton.addEventListener("click", function () {
      const next = activeTheme() === "dark" ? "light" : "dark";
      root.dataset.theme = next;
      try {
        localStorage.setItem("unbse-theme", next);
      } catch (_) {}
      updateThemeButton();
    });
    darkPreference.addEventListener("change", function () {
      if (!root.dataset.theme) updateThemeButton();
    });
  }

  async function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return;
    }
    const fallback = document.createElement("textarea");
    fallback.value = text;
    fallback.setAttribute("readonly", "");
    fallback.style.position = "fixed";
    fallback.style.opacity = "0";
    document.body.appendChild(fallback);
    fallback.select();
    const copied = document.execCommand("copy");
    fallback.remove();
    if (!copied) throw new Error("Copy command was rejected");
  }

  function enhanceCodeBlocks(scope) {
    scope.querySelectorAll("pre[data-copy]:not([data-copy-ready])").forEach(function (pre) {
      pre.dataset.copyReady = "true";
      const wrapper = document.createElement("div");
      wrapper.className = "code-block";
      pre.parentNode.insertBefore(wrapper, pre);
      wrapper.appendChild(pre);

      const button = document.createElement("button");
      button.type = "button";
      button.className = "copy-button";
      button.textContent = "Copy";
      button.setAttribute("aria-label", `Copy ${pre.dataset.copyLabel || "code"}`);
      wrapper.appendChild(button);

      const status = document.createElement("span");
      status.className = "sr-only";
      status.setAttribute("aria-live", "polite");
      wrapper.appendChild(status);

      button.addEventListener("click", async function () {
        try {
          await copyText(pre.textContent);
          button.textContent = "Copied";
          status.textContent = "Copied to clipboard.";
        } catch (_) {
          button.textContent = "Select code";
          status.textContent = "Automatic copy failed. Select the code manually.";
          const range = document.createRange();
          range.selectNodeContents(pre);
          const selection = window.getSelection();
          selection.removeAllRanges();
          selection.addRange(range);
        }
        window.setTimeout(function () {
          button.textContent = "Copy";
        }, 1800);
      });
    });
  }

  enhanceCodeBlocks(document);

  function updateReleaseMetadata(release) {
    document.querySelectorAll("[data-release-version]").forEach(function (node) {
      node.textContent = release.unbseVersion;
    });
    document.querySelectorAll("[data-game-version]").forEach(function (node) {
      node.textContent = release.supportedGame.version;
    });
    document.querySelectorAll("[data-game-store]").forEach(function (node) {
      node.textContent = release.supportedGame.store;
    });
  }

  const referenceRoot = document.querySelector("[data-api-reference]");
  if (!referenceRoot) {
    fetch("api/v1/release.json", { headers: { "Accept": "application/json" } })
      .then(function (response) {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
      })
      .then(updateReleaseMetadata)
      .catch(function () {});
    return;
  }

  const endpointList = referenceRoot.querySelector("[data-endpoint-list]");
  const sdkList = referenceRoot.querySelector("[data-sdk-list]");
  const workflowList = referenceRoot.querySelector("[data-sdk-workflows]");
  const apiStatus = referenceRoot.querySelector("[data-api-status]");
  const searchInput = referenceRoot.querySelector("#api-search");
  const searchCount = referenceRoot.querySelector("[data-search-count]");

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function link(text, href, className) {
    const node = element("a", className, text);
    node.href = href;
    return node;
  }

  function appendText(parent, label, value) {
    const row = element("p", "definition-line");
    const term = element("strong", "", label);
    row.appendChild(term);
    row.appendChild(document.createTextNode(value));
    parent.appendChild(row);
  }

  function codeBlock(code, label) {
    const pre = element("pre");
    pre.dataset.copy = "";
    pre.dataset.copyLabel = label;
    const codeNode = element("code", "", code);
    pre.appendChild(codeNode);
    return pre;
  }

  function resolveReference(spec, reference) {
    if (!reference || !reference.startsWith("#/")) return null;
    return reference.slice(2).split("/").reduce(function (value, key) {
      return value && value[key];
    }, spec);
  }

  function materializePath(path, spec, operation) {
    let concrete = path;
    (operation.parameters || []).forEach(function (parameterEntry) {
      const parameter = parameterEntry.$ref
        ? resolveReference(spec, parameterEntry.$ref)
        : parameterEntry;
      if (!parameter || parameter.in !== "path") return;
      const example = parameter.example ||
        (parameter.schema && parameter.schema.example) ||
        (parameter.schema && parameter.schema.enum && parameter.schema.enum[0]) ||
        parameter.name;
      concrete = concrete.replace(`{${parameter.name}}`, example);
    });
    return concrete;
  }

  function responseSchemaName(spec, response) {
    const content = response && response.content;
    if (!content) return "No response body";
    const mediaType = Object.keys(content)[0];
    const schema = content[mediaType] && content[mediaType].schema;
    if (!schema) return mediaType;
    if (schema.$ref) return `${mediaType} · ${schema.$ref.split("/").pop()}`;
    return `${mediaType} · ${schema.type || "schema"}`;
  }

  function renderParameters(spec, operation) {
    if (!operation.parameters || operation.parameters.length === 0) return null;
    const block = element("div", "parameter-block");
    block.appendChild(element("h4", "", "Path parameters"));
    const scroll = element("div", "table-scroll");
    const table = element("table", "compact-table");
    const head = document.createElement("thead");
    const headRow = document.createElement("tr");
    ["Name", "Required", "Allowed values"].forEach(function (heading) {
      headRow.appendChild(element("th", "", heading));
    });
    head.appendChild(headRow);
    table.appendChild(head);
    const body = document.createElement("tbody");
    operation.parameters.forEach(function (parameterEntry) {
      const parameter = parameterEntry.$ref
        ? resolveReference(spec, parameterEntry.$ref)
        : parameterEntry;
      if (!parameter) return;
      const schema = parameter.schema && parameter.schema.$ref
        ? resolveReference(spec, parameter.schema.$ref)
        : parameter.schema;
      const values = schema && schema.enum ? schema.enum.join(", ") : "—";
      const row = document.createElement("tr");
      row.appendChild(element("td", "", parameter.name));
      row.appendChild(element("td", "", parameter.required ? "Yes" : "No"));
      row.appendChild(element("td", "", values));
      body.appendChild(row);
    });
    table.appendChild(body);
    scroll.appendChild(table);
    block.appendChild(scroll);
    return block;
  }

  function renderEndpoint(spec, method, path, operation, example) {
    const article = element("article", "endpoint-card searchable");
    article.id = `endpoint-${operation.operationId}`;
    article.dataset.search = [
      method,
      path,
      operation.summary,
      operation.description,
      operation.operationId,
      (operation.tags || []).join(" ")
    ].join(" ").toLowerCase();

    const header = element("div", "endpoint-header");
    const route = element("div", "endpoint-route");
    route.appendChild(element("span", `method-badge ${method.toLowerCase()}`, method.toUpperCase()));
    route.appendChild(element("code", "endpoint-path", path));
    header.appendChild(route);
    header.appendChild(element("span", "endpoint-operation", operation.operationId));
    article.appendChild(header);

    const content = element("div", "endpoint-content");
    content.appendChild(element("h3", "", operation.summary));
    if (operation.description) content.appendChild(element("p", "", operation.description));

    const parameters = renderParameters(spec, operation);
    if (parameters) content.appendChild(parameters);

    const concretePath = materializePath(path, spec, operation);
    const baseUrl = (spec.servers && spec.servers[0] && spec.servers[0].url || "").replace(/\/$/, "");
    const requestUrl = `${baseUrl}${concretePath}`;
    const examples = element("div", "request-examples");

    const curl = document.createElement("details");
    curl.open = true;
    curl.appendChild(element("summary", "", "cURL request"));
    curl.appendChild(codeBlock(`curl --fail ${requestUrl}`, `Curl request for ${path}`));
    examples.appendChild(curl);

    const javascript = document.createElement("details");
    javascript.appendChild(element("summary", "", "JavaScript request"));
    javascript.appendChild(codeBlock(
      `const response = await fetch("${requestUrl}");\nif (!response.ok) throw new Error(\`HTTP \${response.status}\`);\nconst data = await response.${path.includes("{headerName}") ? "text" : "json"}();`,
      `JavaScript request for ${path}`
    ));
    examples.appendChild(javascript);
    content.appendChild(examples);

    const responseBlock = element("div", "response-block");
    responseBlock.appendChild(element("h4", "", "Responses"));
    Object.entries(operation.responses || {}).forEach(function (entry) {
      const statusCode = entry[0];
      const responseEntry = entry[1];
      const response = responseEntry.$ref
        ? resolveReference(spec, responseEntry.$ref)
        : responseEntry;
      const row = element("div", "response-row");
      row.appendChild(element("span", statusCode === "200" ? "response-code success" : "response-code", statusCode));
      const responseText = element("div");
      responseText.appendChild(element("strong", "", response.description));
      responseText.appendChild(element("span", "", responseSchemaName(spec, response)));
      row.appendChild(responseText);
      responseBlock.appendChild(row);
    });
    content.appendChild(responseBlock);

    if (example !== undefined) {
      const preview = document.createElement("details");
      preview.className = "response-preview";
      preview.appendChild(element("summary", "", "Current response preview"));
      preview.appendChild(codeBlock(
        typeof example === "string" ? example : JSON.stringify(example, null, 2),
        `Response preview for ${path}`
      ));
      content.appendChild(preview);
    }

    content.appendChild(link(
      path.includes("{headerName}") ? "Open example header" : "Open current response",
      concretePath.replace(/^\//, ""),
      "raw-link"
    ));
    article.appendChild(content);
    return article;
  }

  function renderNamedValues(title, values) {
    if (!values || values.length === 0) return null;
    const details = document.createElement("details");
    details.className = "enum-details";
    details.appendChild(element("summary", "", `${title} (${values.length})`));
    const scroll = element("div", "table-scroll");
    const table = element("table", "compact-table symbol-table");
    const head = document.createElement("thead");
    const headRow = document.createElement("tr");
    headRow.appendChild(element("th", "", "Symbol"));
    headRow.appendChild(element("th", "", "Value"));
    head.appendChild(headRow);
    table.appendChild(head);
    const body = document.createElement("tbody");
    values.forEach(function (item) {
      const row = document.createElement("tr");
      const name = document.createElement("td");
      name.appendChild(element("code", "", item.name));
      row.appendChild(name);
      row.appendChild(element("td", "", String(item.value)));
      body.appendChild(row);
    });
    table.appendChild(body);
    scroll.appendChild(table);
    details.appendChild(scroll);
    return details;
  }

  function renderStructures(structures) {
    if (!structures || structures.length === 0) return null;
    const details = document.createElement("details");
    details.className = "enum-details";
    details.appendChild(element("summary", "", `ABI structures (${structures.length})`));
    const chips = element("div", "structure-list");
    structures.forEach(function (structure) {
      const item = element("div");
      item.appendChild(element("code", "", structure.name));
      item.appendChild(element("span", "", `${structure.sizeBytes} bytes`));
      chips.appendChild(item);
    });
    details.appendChild(chips);
    return details;
  }

  function renderSdkInterface(item) {
    const article = element("article", "sdk-card searchable");
    article.id = `sdk-${item.id}`;
    article.dataset.search = JSON.stringify(item).toLowerCase();

    const heading = element("div", "sdk-heading");
    const title = element("div");
    title.appendChild(element("span", "card-kicker", `ABI v${item.abiVersion}`));
    title.appendChild(element("h3", "", item.name));
    heading.appendChild(title);
    heading.appendChild(link("Download header", `api/v1/${item.header}`, "button compact"));
    article.appendChild(heading);
    article.appendChild(element("p", "sdk-purpose", item.purpose));
    article.appendChild(element("p", "sdk-use", item.whenToUse));

    const query = element("div", "query-export");
    query.appendChild(element("span", "definition-label", "Query export"));
    query.appendChild(codeBlock(item.querySignature, `${item.queryExport} signature`));
    article.appendChild(query);

    const functions = element("div", "function-list");
    functions.appendChild(element("h4", "", item.functions.length ? "Service-table functions" : "Service-table functions"));
    if (item.functions.length === 0) {
      functions.appendChild(element("p", "empty-note", "Query-only interface: the export fills the runtime identity structure directly."));
    } else {
      item.functions.forEach(function (fn) {
        const block = element("div", "function-entry");
        block.appendChild(element("h5", "", fn.name));
        block.appendChild(codeBlock(fn.signature, `${fn.name} signature`));
        appendText(block, "Returns: ", fn.returns);
        appendText(block, "Use: ", fn.use);
        functions.appendChild(block);
      });
    }
    article.appendChild(functions);

    if (item.callbacks && item.callbacks.length) {
      const callbacks = element("div", "function-list callback-list");
      callbacks.appendChild(element("h4", "", "Host-invoked callbacks"));
      item.callbacks.forEach(function (callback) {
        const block = element("div", "function-entry");
        block.appendChild(element("h5", "", callback.name));
        block.appendChild(codeBlock(callback.signature, `${callback.name} signature`));
        appendText(block, "Returns: ", callback.returns);
        appendText(block, "Use: ", callback.use);
        callbacks.appendChild(block);
      });
      article.appendChild(callbacks);
    }

    const detailsGrid = element("div", "sdk-details-grid");
    [
      ["Capabilities", item.capabilities],
      ["Declared effects", item.declaredEffects],
      ["Compatibility values", item.compatibilityValues],
      ["Core messages", item.coreMessages],
      ["Value types", item.valueTypes],
      ["Function flags", item.functionFlags],
      ["Identity flags", item.identityFlags],
      ["Platforms", item.platforms],
      ["Modules", item.modules],
      ["Result codes", item.resultCodes]
    ].forEach(function (entry) {
      const detail = renderNamedValues(entry[0], entry[1]);
      if (detail) detailsGrid.appendChild(detail);
    });
    const structures = renderStructures(item.structures);
    if (structures) detailsGrid.appendChild(structures);
    article.appendChild(detailsGrid);
    return article;
  }

  function renderWorkflows(workflows) {
    if (!workflows || workflows.length === 0) return;
    const heading = element("div", "subsection-heading");
    heading.appendChild(element("h3", "", "Modder workflows"));
    heading.appendChild(element("p", "", "Recommended call order for common add-on jobs."));
    workflowList.appendChild(heading);
    const grid = element("div", "workflow-reference-grid");
    workflows.forEach(function (workflow) {
      const card = element("article");
      card.appendChild(element("h4", "", workflow.title));
      const chain = element("ol", "call-chain");
      workflow.calls.forEach(function (call) {
        const item = document.createElement("li");
        item.appendChild(element("code", "", call));
        chain.appendChild(item);
      });
      card.appendChild(chain);
      card.appendChild(element("p", "", workflow.rule));
      grid.appendChild(card);
    });
    workflowList.appendChild(grid);
  }

  function updateSearchCount() {
    const items = Array.from(referenceRoot.querySelectorAll(".searchable"));
    const visible = items.filter(function (item) {
      return !item.hidden;
    }).length;
    searchCount.textContent = `${visible} of ${items.length} reference sections`;
  }

  function installSearch() {
    searchInput.addEventListener("input", function () {
      const query = searchInput.value.trim().toLowerCase();
      referenceRoot.querySelectorAll(".searchable").forEach(function (item) {
        item.hidden = Boolean(query) && !item.dataset.search.includes(query);
      });
      updateSearchCount();
    });
    updateSearchCount();
  }

  async function fetchJson(path) {
    const response = await fetch(path, { headers: { "Accept": "application/json" } });
    if (!response.ok) throw new Error(`${path} returned HTTP ${response.status}`);
    return response.json();
  }

  async function loadReference() {
    try {
      const [
        spec,
        discovery,
        catalog,
        release,
        interfaces,
        guide,
        headers
      ] = await Promise.all([
        fetchJson("api/v1/openapi.json"),
        fetchJson("api/index.json"),
        fetchJson("api/v1/index.json"),
        fetchJson("api/v1/release.json"),
        fetchJson("api/v1/interfaces.json"),
        fetchJson("api/v1/plugin-guide.json"),
        fetchJson("api/v1/headers/index.json")
      ]);

      const responseExamples = {
        "/api/index.json": discovery,
        "/api/v1/index.json": catalog,
        "/api/v1/release.json": release,
        "/api/v1/interfaces.json": interfaces,
        "/api/v1/plugin-guide.json": guide,
        "/api/v1/headers/index.json": headers,
        "/api/v1/headers/{headerName}": "#pragma once\n\n// See the selected canonical SDK header for its complete declarations.\n"
      };

      Object.entries(spec.paths).forEach(function (pathEntry) {
        const path = pathEntry[0];
        Object.entries(pathEntry[1]).forEach(function (operationEntry) {
          const method = operationEntry[0];
          const operation = operationEntry[1];
          endpointList.appendChild(renderEndpoint(
            spec,
            method,
            path,
            operation,
            responseExamples[path]
          ));
        });
      });

      renderWorkflows(interfaces.workflows);
      interfaces.interfaces.forEach(function (item) {
        sdkList.appendChild(renderSdkInterface(item));
      });

      updateReleaseMetadata(release);

      apiStatus.textContent = `${Object.keys(spec.paths).length} HTTP endpoints and ${interfaces.interfaces.length} native interfaces loaded from API ${interfaces.sdkVersion}.`;
      apiStatus.classList.add("loaded");
      enhanceCodeBlocks(referenceRoot);
      installSearch();
    } catch (error) {
      apiStatus.classList.add("error");
      apiStatus.textContent = `The generated reference could not load: ${error.message}. Use the raw OpenAPI and SDK JSON links on this page.`;
      searchCount.textContent = "Reference unavailable";
    }
  }

  loadReference();
})();
