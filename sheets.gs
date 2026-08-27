function doGet(e) {
  try {
    var spreadsheet = getSpreadsheet_();
    var settings = readSettings_(spreadsheet);
    var timezone = settings.timezone || 'America/Sao_Paulo';
    var requestedDate = e && e.parameter && e.parameter.date
      ? normaliseDate_(e.parameter.date, timezone)
      : Utilities.formatDate(new Date(), timezone, 'yyyy-MM-dd');
    if (e && e.parameter && e.parameter.date && !requestedDate) {
      throw new Error('Invalid date parameter: ' + e.parameter.date);
    }

    var response = {
      settings: settings,
      letter: pickDailyNote_(spreadsheet, requestedDate, timezone),
      memory: pickDatedRow_(spreadsheet, 'memories', requestedDate, ['date', 'image_file', 'caption'], timezone),
    };

    return ContentService
      .createTextOutput(JSON.stringify(response))
      .setMimeType(ContentService.MimeType.JSON);
  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({ error: error.message }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

function getSpreadsheet_() {
  var spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
  if (spreadsheet) {
    return spreadsheet;
  }

  if (typeof SPREADSHEET_ID !== 'undefined' && String(SPREADSHEET_ID).trim()) {
    return SpreadsheetApp.openById(String(SPREADSHEET_ID).trim());
  }

  throw new Error('No active spreadsheet and SPREADSHEET_ID is not configured.');
}

function readSettings_(spreadsheet) {
  var settings = {
    timezone: 'America/Sao_Paulo',
    location_name: 'Botucatu, SP',
    latitude: -22.8858,
    longitude: -48.4450,
    next_visit_date: '2026-06-24',
    next_visit_label: 'próxima visita',
  };

  var sheet = spreadsheet.getSheetByName('settings');
  if (!sheet) {
    return settings;
  }

  var values = sheet.getDataRange().getValues();
  for (var i = 1; i < values.length; i++) {
    var key = String(values[i][0]).trim();
    if (!key) {
      continue;
    }
    var value = values[i][1];
    settings[key] = Object.prototype.toString.call(value) === '[object Date]'
      ? normaliseDate_(value, settings.timezone)
      : value;
  }

  return settings;
}

function pickDailyNote_(spreadsheet, requestedDate, timezone) {
  var sheet = spreadsheet.getSheetByName('daily_notes');
  if (!sheet) {
    return pickDatedRow_(spreadsheet, 'letters', requestedDate, ['date', 'title', 'body'], timezone);
  }

  var values = sheet.getDataRange().getValues();
  if (values.length < 2) return null;

  var headers = values[0].map(function(h) { return String(h).trim().toLowerCase(); });
  var msgIndex = headers.indexOf('message');
  if (msgIndex === -1) return null;

  var rows = values.slice(1).filter(function(r) {
    return String(r[msgIndex]).trim().length > 0;
  });
  if (rows.length === 0) return null;

  var epoch = new Date(0);
  var today = requestedDate
    ? new Date(requestedDate + 'T12:00:00')
    : new Date();
  var dayIndex = Math.floor((today - epoch) / 86400000);
  var row = rows[((dayIndex % rows.length) + rows.length) % rows.length];

  return { body: String(row[msgIndex]).trim(), title: '', date: requestedDate || '' };
}

function pickDatedRow_(spreadsheet, sheetName, requestedDate, columns, timezone) {
  var sheet = spreadsheet.getSheetByName(sheetName);
  if (!sheet) {
    return null;
  }

  var values = sheet.getDataRange().getValues();
  if (values.length < 2) {
    return null;
  }

  var headers = values[0].map(function (header) {
    return String(header).trim().toLowerCase();
  });
  var dateIndex = headers.indexOf('date');
  if (dateIndex === -1) {
    return null;
  }

  var bestDate = '';
  var bestRow = null;
  for (var i = 1; i < values.length; i++) {
    var rowDate = normaliseDate_(values[i][dateIndex], timezone);
    if (rowDate && rowDate <= requestedDate && rowDate >= bestDate) {
      bestDate = rowDate;
      bestRow = values[i];
    }
  }

  if (!bestRow) {
    return null;
  }

  var result = {};
  for (var j = 0; j < columns.length; j++) {
    var column = String(columns[j]).trim().toLowerCase();
    var index = headers.indexOf(column);
    result[column] = column === 'date'
      ? bestDate
      : index === -1
        ? ''
        : bestRow[index];
  }

  return result;
}

function normaliseDate_(value, timezone) {
  if (!value) {
    return '';
  }

  var date;
  if (Object.prototype.toString.call(value) === '[object Date]') {
    date = value;
  } else {
    var text = String(value).trim();
    if (!text) {
      return '';
    }

    if (/^\d{4}-\d{2}-\d{2}$/.test(text)) {
      var parts = text.split('-');
      var year = Number(parts[0]);
      var month = Number(parts[1]);
      var day = Number(parts[2]);
      var maxDay = new Date(year, month, 0).getDate();
      if (month < 1 || month > 12 || day < 1 || day > maxDay) {
        return '';
      }
      return text;
    }

    date = new Date(text);
  }

  if (isNaN(date.getTime())) {
    return '';
  }

  return Utilities.formatDate(date, timezone || 'America/Sao_Paulo', 'yyyy-MM-dd');
}
