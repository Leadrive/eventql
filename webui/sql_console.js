ZFE.views["io.zscale.zfe.sql_console"] = function(elem, params) {
  var query_mgr = EventSourceHandler();
  var history = [];

  this.initialize = function(params) {
    var page = zTemplateUtil.getTemplate("zscale_sql_console_main_tpl");

    //init clear console
    zDomUtil.onClick(page.querySelector("button.clear"), clearConsole);
    zDomUtil.handleLinks(page, ZFE.navigateTo);
    elem.appendChild(page);

    initPrompt();
    displayBanner();
    fetchHistory();
  };

  this.destroy = function() {
    query_mgr.closeAll();
  };

  var initPrompt = function() {
    var prmpt = elem.querySelector(".prompt textarea");
    focusPrompt();

    var ctrl_pressed = false;
    var cmd_pressed = false;
    var history_idx = 0;

    var resize_input = function() {
      prmpt.setAttribute("rows", prmpt.value.split("\n").length);
    };

    var history_prev = function() {
      if (history_idx >= history.length) {
        return;
      }

      history_idx++;
      prmpt.value = history[history.length - history_idx].content;
      resize_input();

      var cursor_pos = prmpt.value.indexOf("\n");
      if (cursor_pos < 0) {
        cursor_pos = prmpt.value.length;
      }

      zDomUtil.textareaSetCursor(prmpt, cursor_pos);
    };

    var history_next = function() {
      if (history_idx == 0) {
        return;
      }

      history_idx--;
      if (history_idx == 0) {
        prmpt.value = "";
      } else {
        prmpt.value = history[history.length - history_idx].content;
      }

      resize_input();
      zDomUtil.textareaSetCursor(prmpt, prmpt.value.length);
    };

    var execute_prompt = function() {
      var query_sql = prmpt.value;
      if (query_sql.length == 0) {
        return;
      }

      executeQuery(query_sql);

      if (history.length == 0 ||
          history[history.length - 1].content != query_sql) {
        history.push({
          content: query_sql,
          time: (new Date()).getTime()
        });

        params.app.api_stubs.workspace.appendToConsoleHistory({
          project_id: params.project_id,
          content: query_sql
        });
      }

      history_idx = 0;
      prmpt.value = "";
      elem.querySelector(".prompt .hint").style.display = "none";
      scrollConsoleToBottom();
    };

    prmpt.addEventListener("keydown", function(e) {
      switch (e.keyCode) {
        case 13: // ENTER
          if (e.keyCode == 13 && !(ctrl_pressed || cmd_pressed)) { // ENTER
            execute_prompt();
            e.preventDefault();
            e.stopPropagation();
          }
          break;
        case 17: // CTRL
          ctrl_pressed = true;
          break;
        case 91: // CMD
          cmd_pressed = true;
          break;
        case 38: // UP
          if (prmpt.value.indexOf("\n") < 0 ||
              zDomUtil.textareaGetCursor(prmpt) <= prmpt.value.indexOf("\n")) {
            history_prev();
            e.preventDefault();
            e.stopPropagation();
          }
          cmd_pressed = false;
          ctrl_pressed = false;
          break;
        case 40: // DOWN
          if (prmpt.value.indexOf("\n") < 0 ||
              zDomUtil.textareaGetCursor(prmpt) >= prmpt.value.lastIndexOf("\n")) {
            history_next();
            e.preventDefault();
            e.stopPropagation();
          }
          cmd_pressed = false;
          ctrl_pressed = false;
          break;
      }
    });

    prmpt.addEventListener("keyup", function(e) {
      switch (e.keyCode) {
        case 17: // CTRL
          ctrl_pressed = false;
          break;
        case 91: // CMD
          cmd_pressed = false;
          break;
      }

      resize_input();
    });

    prmpt.addEventListener("change", resize_input, false);
  }

  var focusPrompt = function() {
    elem.querySelector(".prompt textarea").focus();
  };

  var scrollConsoleToBottom = function() {
    var sql_console = elem.querySelector(".console");
    sql_console.scrollTop = sql_console.scrollHeight;
  };

  var executeQuery = function(query_string) {
    var query_elem = zTemplateUtil.getTemplate("zscale_sql_console_query_tpl");
    query_elem.querySelector(".cmd").innerHTML =
        "zsql&gt; " + zDomUtil.escapeHTML(query_string);

    query_elem = appendConsoleEntry(query_elem);

    var status_elem = query_elem.querySelector(".status");

    var query_id = Math.random().toString(36);
    var query = query_mgr.add(
      query_id,
      ZFE.api_stubs.z1.executeSQLSSE(query_string));

    query.addEventListener('result', function(e) {
      query_mgr.close(query_id);
      var data = JSON.parse(e.data);
      status_elem.classList.add("success");
      renderQueryResult(query_elem, data);
    });

    query.addEventListener('query_error', function(e) {
      query_mgr.close(query_id);
      var err = JSON.parse(e.data).error;
      status_elem.innerHTML = zDomUtil.escapeHTML(err);
      status_elem.classList.add("error");
    });

    query.addEventListener('error', function(e) {
      query_mgr.close(query_id);
      status_elem.innerHTML = "Server Error";
      status_elem.classList.add("error");
    });

    query.addEventListener('status', function(e) {
      var p = JSON.parse(e.data).progress * 100;
      status_elem.innerHTML = "Query running - " + p.toFixed(2) + "%";
    });
  }

  var renderQueryResult = function(query_elem, data) {
    data.results.forEach(function(result) {
      switch (result.type) {
        case "chart":
          var chart = document.createElement("div");
          chart.className = "chart";
          chart.innerHTML = result.svg;
          query_elem.appendChild(chart);
          break;
        case "table":
          var tbl = document.createElement("table");
          var tbl_head = document.createElement("thead");
          tbl.appendChild(tbl_head);
          var tbl_head_tr = document.createElement("tr");
          tbl_head.appendChild(tbl_head_tr);
          var tbl_body = document.createElement("thead");
          tbl.appendChild(tbl_body);

          result.columns.forEach(function(c) {
            var th = document.createElement("th");
            th.innerHTML = zDomUtil.escapeHTML(c);
            tbl_head_tr.appendChild(th);
          });

          result.rows.forEach(function(row) {
            var tr = document.createElement("tr");
            tbl_body.appendChild(tr);

            row.forEach(function(val) {
              var td = document.createElement("td");
              td.innerHTML = zDomUtil.escapeHTML(val);
              tr.appendChild(td);
            });
          });

          query_elem.appendChild(tbl);
          break;
      }
    });
  }

  var fetchHistory = function() {
    params.app.api_stubs.workspace.fetchConsoleHistory({
      project_id: params.project_id
    }, function(r) {
      if (r.success) {
        history = r.result.history.reverse();
      } else {
        var errmsg = document.createElement("div");
        var errmsg_p = document.createElement("p");
        errmsg.appendChild(errmsg_p);
        errmsg_p.innerHTML = "Unable to fetch history &mdash; old entries will not be available";
        errmsg_p.classList.add("warn");
        appendConsoleEntry(errmsg);
      }

      elem.querySelector(".loader").classList.add("hidden");
    });
  };

  var clearConsole = function() {
    elem.querySelector(".console").innerHTML = "";
    displayBanner();
    focusPrompt();
    query_mgr.closeAll();
  };

  var displayBanner = function() {
    var user_str = params.user_info.user_name + " <" + params.user_info.user_id + ">";
    var banner = document.createElement("div");
    var banner_p = document.createElement("p");
    banner.appendChild(banner_p);
    banner_p.innerHTML = ">> zsql v0.2.3 &mdash; logged in as " + zDomUtil.escapeHTML(user_str);
    banner_p.classList.add("banner");
    appendConsoleEntry(banner);
  };

  var appendConsoleEntry = function(e) {
    var console_elem = elem.querySelector(".console");
    console_elem.insertBefore(e, console_elem.firstChild);
    return console_elem.firstElementChild;
  }

};
