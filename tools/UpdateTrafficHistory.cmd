@echo off
setlocal
chcp 65001 >nul
set "TM_HISTORY_TOOL=%~f0"
title Update Traffic History

echo Update Traffic History
echo.
echo This tool directly converts all published GitHub history formats to the latest format.
echo Already updated history is detected automatically and left unchanged.
echo TrafficMonitor must be closed only when an adjustment is required.
echo.
powershell.exe -NoProfile -Command "Write-Host (-join [char[]](0x6CE8,0x610F,0xFF1A,0x4F7F,0x7528,0x524D,0x52A1,0x5FC5,0x81EA,0x884C,0x5907,0x4EFD,0x3002))"
echo NOTICE: You must back up your traffic history before use.
echo.
set "CONFIRM=Y"
if not "%TM_HISTORY_ASSUME_YES%"=="1" set "CONFIRM="
if not "%TM_HISTORY_ASSUME_YES%"=="1" powershell.exe -NoProfile -Command "Write-Host (-join [char[]](0x8F93,0x5165,0x20,0x59,0x20,0x8FDB,0x5165,0x4E0B,0x4E00,0x9636,0x6BB5,0x3002))"
if not "%TM_HISTORY_ASSUME_YES%"=="1" set /p "CONFIRM=Enter Y to continue: "
if /I not "%CONFIRM%"=="Y" (
    echo.
    powershell.exe -NoProfile -Command "Write-Host (-join [char[]](0x5DF2,0x53D6,0x6D88,0xFF0C,0x672A,0x4FEE,0x6539,0x4EFB,0x4F55,0x6587,0x4EF6,0x3002))"
    echo Cancelled. No files were changed.
    echo.
    if not "%TM_HISTORY_NO_PAUSE%"=="1" pause
    exit /b 0
)

echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$raw=[IO.File]::ReadAllText($env:TM_HISTORY_TOOL); $marker='# POWERSHELL_PAYLOAD'; $start=$raw.LastIndexOf($marker); if($start -lt 0){throw 'Embedded migration payload was not found.'}; & ([scriptblock]::Create($raw.Substring($start+$marker.Length)))"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if not "%EXIT_CODE%"=="0" (
    echo Failed. Error code: %EXIT_CODE%
    echo Existing history files were kept when an operation failed.
)
echo.
if not "%TM_HISTORY_NO_PAUSE%"=="1" pause
exit /b %EXIT_CODE%

# POWERSHELL_PAYLOAD
$script:embeddedMigrationSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public static class TrafficHistoryMigration
{
    private const int LatestHistoryFormat = 8;
    private const int LatestArchiveFormat = 2;
    private const int LatestMigrationFormat = 9;
    private const int LatestDictionaryVersion = 4;
    private const int SummaryVersion = 1;
    private const ulong AbnormalMinuteThreshold = 2UL * 1024UL * 1024UL * 1024UL;
    private const string Base62Digits = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
    private static readonly Regex DailyName = new Regex(@"^\d{4}-\d{2}-\d{2}\.tsv$", RegexOptions.CultureInvariant);

    private sealed class Amount
    {
        public ulong Rx;
        public ulong Tx;
    }

    private sealed class SummaryData
    {
        public readonly SortedDictionary<ulong, Amount> ById = new SortedDictionary<ulong, Amount>();
        public ulong Rx;
        public ulong Tx;
    }

    private sealed class AppDictionary
    {
        public readonly Dictionary<string, ulong> ByName = new Dictionary<string, ulong>(StringComparer.Ordinal);
        public readonly SortedDictionary<ulong, string> ById = new SortedDictionary<ulong, string>();
        public ulong NextId = 1;

        public ulong GetOrAdd(string name)
        {
            ulong id;
            if (ByName.TryGetValue(name, out id))
            {
                return id;
            }
            if (NextId == ulong.MaxValue)
            {
                throw new InvalidDataException("The application dictionary is full.");
            }
            id = NextId++;
            ByName.Add(name, id);
            ById.Add(id, name);
            return id;
        }
    }

    private sealed class DayResult
    {
        public DateTime Date;
        public string FilePath;
        public long InputLines;
        public long OutputLines;
        public long Removed;
        public SummaryData Summary;
        public ulong Generation;
    }

    private sealed class SummaryItem
    {
        public DateTime Start;
        public SummaryData Summary;
        public ulong Generation;
    }

    private sealed class SummaryCounts
    {
        public int Days;
        public int Weeks;
        public int Months;
        public int Quarters;
    }

    public static int Run(string baseDirectory, bool allowRunning)
    {
        baseDirectory = Path.GetFullPath(baseDirectory);
        string historyDirectory = Path.Combine(baseDirectory, "app_traffic_history");
        string legacySingle = Path.Combine(baseDirectory, "app_traffic_history.tsv");
        string statePath = Path.Combine(baseDirectory, "app_traffic_state.tsv");

        if (IsLatestLayout(historyDirectory, legacySingle, statePath))
        {
            Console.WriteLine("Latest format detected. No adjustment is required.");
            return 0;
        }
        if (!allowRunning && Process.GetProcessesByName("TrafficMonitor").Length != 0)
        {
            throw new InvalidOperationException("TrafficMonitor is running. Exit TrafficMonitor, then run this launcher again.");
        }

        bool hasSingle = File.Exists(legacySingle);
        SortedDictionary<DateTime, List<string>> dailySources = FindPublishedDailySources(historyDirectory);
        if (!hasSingle && dailySources.Count == 0)
        {
            throw new InvalidDataException("No history generated by a published GitHub version was found.");
        }
        if (File.Exists(Path.Combine(historyDirectory, "apps.tsv")))
        {
            throw new InvalidDataException("An unpublished intermediate application dictionary was found. This tool accepts published GitHub formats and the latest format only.");
        }

        Console.WriteLine("TrafficMonitor directory: " + baseDirectory);
        Console.WriteLine(hasSingle && dailySources.Count != 0
            ? "Published format detected: mixed upgrade state (single file plus V1.1.1 daily files)."
            : hasSingle
                ? "Published format detected: v0.1.0 - V1.1.0 single history file."
                : "Published format detected: V1.1.1 split daily files (including old)."
        );

        string stagingDirectory = Path.Combine(baseDirectory, ".traffic_history_staging_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(stagingDirectory);
        bool activated = false;
        try
        {
            if (hasSingle)
            {
                string splitDirectory = Path.Combine(stagingDirectory, ".legacy_split");
                MergeDailySources(dailySources, SplitLegacySingleFile(legacySingle, splitDirectory));
            }

            Console.WriteLine("Converting history in one pass...");
            AppDictionary dictionary = new AppDictionary();
            List<DayResult> dayResults = new List<DayResult>();
            long inputLines = 0;
            long outputLines = 0;
            long removedRows = 0;
            ulong allRx = 0;
            ulong allTx = 0;

            foreach (KeyValuePair<DateTime, List<string>> entry in dailySources)
            {
                DayResult result = ConvertDay(entry.Key, entry.Value, stagingDirectory, dictionary);
                dayResults.Add(result);
                inputLines += result.InputLines;
                outputLines += result.OutputLines;
                removedRows += result.Removed;
                allRx = AddChecked(allRx, result.Summary.Rx);
                allTx = AddChecked(allTx, result.Summary.Tx);
                Console.WriteLine("  {0:yyyy-MM-dd}: {1} -> {2} row(s), removed {3} abnormal row(s)",
                    result.Date, result.InputLines, result.OutputLines, result.Removed);
            }

            string splitPath = Path.Combine(stagingDirectory, ".legacy_split");
            if (Directory.Exists(splitPath))
            {
                Directory.Delete(splitPath, true);
            }

            WriteAppDictionary(Path.Combine(stagingDirectory, "apps.tsv"), dictionary);
            SummaryCounts summaryCounts = BuildSummaries(stagingDirectory, dayResults);
            ValidateOutput(stagingDirectory, dayResults.Count, dictionary.ById.Count);

            if (!allowRunning && Process.GetProcessesByName("TrafficMonitor").Length != 0)
            {
                throw new InvalidOperationException("TrafficMonitor started during migration. Exit it and run the launcher again.");
            }

            string backupPath = Activate(
                baseDirectory,
                historyDirectory,
                legacySingle,
                statePath,
                stagingDirectory,
                allRx,
                allTx);
            activated = true;

            Console.WriteLine("Converted {0} input row(s) to {1} final row(s).", inputLines, outputLines);
            Console.WriteLine("Removed {0} row(s) whose per-minute application total exceeded 2 GiB.", removedRows);
            Console.WriteLine("Applications: " + dictionary.ById.Count);
            Console.WriteLine("Summaries: {0} day, {1} week, {2} month, {3} quarter.",
                summaryCounts.Days, summaryCounts.Weeks, summaryCounts.Months, summaryCounts.Quarters);
            Console.WriteLine("all_time: {0} / {1}", allRx, allTx);
            Console.WriteLine("Traffic history is now in the latest format.");
            Console.WriteLine("Safety backup retained: " + backupPath);
            return 0;
        }
        finally
        {
            if (!activated && Directory.Exists(stagingDirectory))
            {
                try { Directory.Delete(stagingDirectory, true); }
                catch { }
            }
        }
    }

    private static bool IsLatestLayout(string historyDirectory, string legacySingle, string statePath)
    {
        if (File.Exists(legacySingle) || !Directory.Exists(historyDirectory))
        {
            return false;
        }
        if (ReadStateFormat(statePath, "history_format") != LatestHistoryFormat ||
            ReadStateFormat(statePath, "history_archive_format") != LatestArchiveFormat ||
            ReadStateFormat(statePath, "history_migration_format") != LatestMigrationFormat)
        {
            return false;
        }
        string dictionaryPath = Path.Combine(historyDirectory, "apps.tsv");
        if (!File.Exists(dictionaryPath))
        {
            return false;
        }
        using (StreamReader reader = OpenReader(dictionaryPath))
        {
            if (!string.Equals(reader.ReadLine(), "version\t" + LatestDictionaryVersion, StringComparison.Ordinal))
            {
                return false;
            }
        }
        return FindPublishedDailySources(historyDirectory).Count == 0;
    }

    private static int ReadStateFormat(string path, string key)
    {
        if (!File.Exists(path))
        {
            return 0;
        }
        foreach (string line in File.ReadLines(path, Utf8))
        {
            int separator = line.IndexOf('\t');
            int value;
            if (separator > 0 && string.Equals(line.Substring(0, separator), key, StringComparison.Ordinal) &&
                int.TryParse(line.Substring(separator + 1), NumberStyles.None, CultureInfo.InvariantCulture, out value))
            {
                return value;
            }
        }
        return 0;
    }

    private static SortedDictionary<DateTime, List<string>> FindPublishedDailySources(string historyDirectory)
    {
        SortedDictionary<DateTime, List<string>> result = new SortedDictionary<DateTime, List<string>>();
        AddDailyFiles(result, historyDirectory);
        AddDailyFiles(result, Path.Combine(historyDirectory, "old"));
        return result;
    }

    private static void AddDailyFiles(SortedDictionary<DateTime, List<string>> result, string directory)
    {
        if (!Directory.Exists(directory))
        {
            return;
        }
        foreach (string path in Directory.GetFiles(directory, "*.tsv", SearchOption.TopDirectoryOnly))
        {
            string name = Path.GetFileName(path);
            if (!DailyName.IsMatch(name))
            {
                continue;
            }
            DateTime date;
            if (!DateTime.TryParseExact(Path.GetFileNameWithoutExtension(name), "yyyy-MM-dd",
                CultureInfo.InvariantCulture, DateTimeStyles.None, out date))
            {
                continue;
            }
            List<string> paths;
            if (!result.TryGetValue(date.Date, out paths))
            {
                paths = new List<string>();
                result.Add(date.Date, paths);
            }
            paths.Add(path);
        }
        foreach (List<string> paths in result.Values)
        {
            paths.Sort(StringComparer.OrdinalIgnoreCase);
        }
    }

    private static void MergeDailySources(
        SortedDictionary<DateTime, List<string>> target,
        SortedDictionary<DateTime, List<string>> source)
    {
        foreach (KeyValuePair<DateTime, List<string>> entry in source)
        {
            List<string> paths;
            if (!target.TryGetValue(entry.Key, out paths))
            {
                paths = new List<string>();
                target.Add(entry.Key, paths);
            }
            paths.AddRange(entry.Value);
            paths.Sort(StringComparer.OrdinalIgnoreCase);
        }
    }

    private static SortedDictionary<DateTime, List<string>> SplitLegacySingleFile(string sourcePath, string outputDirectory)
    {
        Directory.CreateDirectory(outputDirectory);
        Dictionary<string, StreamWriter> writers = new Dictionary<string, StreamWriter>(StringComparer.Ordinal);
        long lineNumber = 0;
        try
        {
            using (StreamReader reader = OpenReader(sourcePath))
            {
                string line;
                while ((line = reader.ReadLine()) != null)
                {
                    if (string.IsNullOrWhiteSpace(line))
                    {
                        continue;
                    }
                    lineNumber++;
                    DateTime date;
                    int minute;
                    string appName;
                    ulong rx;
                    ulong tx;
                    ParseSingleRow(sourcePath, lineNumber, line, out date, out minute, out appName, out rx, out tx);
                    string key = date.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
                    StreamWriter writer;
                    if (!writers.TryGetValue(key, out writer))
                    {
                        writer = OpenWriter(Path.Combine(outputDirectory, key + ".tsv"));
                        writers.Add(key, writer);
                    }
                    writer.Write(FormatMinute(minute));
                    writer.Write('\t');
                    writer.Write(SanitizeField(appName));
                    writer.Write('\t');
                    writer.Write(rx.ToString(CultureInfo.InvariantCulture));
                    writer.Write('\t');
                    writer.WriteLine(tx.ToString(CultureInfo.InvariantCulture));
                }
            }
        }
        finally
        {
            foreach (StreamWriter writer in writers.Values)
            {
                writer.Dispose();
            }
        }
        if (writers.Count == 0)
        {
            throw new InvalidDataException("The legacy single history file contains no valid rows.");
        }
        SortedDictionary<DateTime, List<string>> result = new SortedDictionary<DateTime, List<string>>();
        AddDailyFiles(result, outputDirectory);
        return result;
    }

    private static void ParseSingleRow(
        string path,
        long lineNumber,
        string line,
        out DateTime date,
        out int minute,
        out string appName,
        out ulong rx,
        out ulong tx)
    {
        string[] fields = line.Split('\t');
        if (fields.Length < 4)
        {
            throw InvalidRow(path, lineNumber, line);
        }
        appName = fields[fields.Length - 3];
        if (string.IsNullOrWhiteSpace(appName) ||
            !TryParseAmount(fields[fields.Length - 2], out rx) ||
            !TryParseAmount(fields[fields.Length - 1], out tx))
        {
            throw InvalidRow(path, lineNumber, line);
        }
        date = DateTime.MinValue;
        minute = -1;
        for (int index = 0; index < fields.Length - 3; index++)
        {
            DateTime candidateDate;
            int candidateMinute;
            if (TryParseDateTimeMinute(fields[index], out candidateDate, out candidateMinute))
            {
                date = candidateDate;
                minute = candidateMinute;
            }
        }
        if (date == DateTime.MinValue || minute < 0)
        {
            throw InvalidRow(path, lineNumber, line);
        }
    }

    private static DayResult ConvertDay(
        DateTime date,
        List<string> sourcePaths,
        string historyDirectory,
        AppDictionary dictionary)
    {
        SortedDictionary<int, Dictionary<string, Amount>> minutes =
            new SortedDictionary<int, Dictionary<string, Amount>>();
        long inputLines = 0;
        foreach (string sourcePath in sourcePaths)
        {
            int currentMinute = -1;
            long sourceLine = 0;
            using (StreamReader reader = OpenReader(sourcePath))
            {
                string line;
                while ((line = reader.ReadLine()) != null)
                {
                    if (string.IsNullOrWhiteSpace(line))
                    {
                        continue;
                    }
                    sourceLine++;
                    string appName;
                    ulong rx;
                    ulong tx;
                    ParseDailyRow(sourcePath, sourceLine, line, date, ref currentMinute, out appName, out rx, out tx);
                    Dictionary<string, Amount> apps;
                    if (!minutes.TryGetValue(currentMinute, out apps))
                    {
                        apps = new Dictionary<string, Amount>(StringComparer.Ordinal);
                        minutes.Add(currentMinute, apps);
                    }
                    Amount amount;
                    if (!apps.TryGetValue(appName, out amount))
                    {
                        amount = new Amount();
                        apps.Add(appName, amount);
                    }
                    amount.Rx = AddChecked(amount.Rx, rx);
                    amount.Tx = AddChecked(amount.Tx, tx);
                    inputLines++;
                }
            }
        }

        string outputPath = GetDailyPath(historyDirectory, date);
        Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
        SummaryData summary = new SummaryData();
        long outputLines = 0;
        long removed = 0;
        using (StreamWriter writer = OpenWriter(outputPath))
        {
            foreach (KeyValuePair<int, Dictionary<string, Amount>> minuteEntry in minutes)
            {
                List<string> names = new List<string>(minuteEntry.Value.Keys);
                names.Sort(StringComparer.Ordinal);
                bool first = true;
                foreach (string name in names)
                {
                    Amount amount = minuteEntry.Value[name];
                    if (IsAbnormal(amount.Rx, amount.Tx))
                    {
                        removed++;
                        continue;
                    }
                    ulong id = dictionary.GetOrAdd(name);
                    writer.Write(first ? FormatMinute(minuteEntry.Key) : string.Empty);
                    writer.Write('\t');
                    writer.Write(ToBase62(id));
                    writer.Write('\t');
                    writer.Write(ToBase62(amount.Rx));
                    writer.Write('\t');
                    writer.WriteLine(ToBase62(amount.Tx));
                    first = false;
                    outputLines++;
                    AddSummary(summary, id, amount.Rx, amount.Tx);
                }
            }
        }
        FileInfo file = new FileInfo(outputPath);
        return new DayResult
        {
            Date = date,
            FilePath = outputPath,
            InputLines = inputLines,
            OutputLines = outputLines,
            Removed = removed,
            Summary = summary,
            Generation = unchecked((ulong)file.Length ^ (ulong)file.LastWriteTimeUtc.Ticks)
        };
    }

    private static void ParseDailyRow(
        string path,
        long lineNumber,
        string line,
        DateTime expectedDate,
        ref int currentMinute,
        out string appName,
        out ulong rx,
        out ulong tx)
    {
        string[] fields = line.Split('\t');
        if (fields.Length < 4)
        {
            throw InvalidRow(path, lineNumber, line);
        }
        appName = fields[fields.Length - 3];
        if (string.IsNullOrWhiteSpace(appName) ||
            !TryParseAmount(fields[fields.Length - 2], out rx) ||
            !TryParseAmount(fields[fields.Length - 1], out tx))
        {
            throw InvalidRow(path, lineNumber, line);
        }

        int parsedMinute = -1;
        for (int index = 0; index < fields.Length - 3; index++)
        {
            int candidateMinute;
            DateTime candidateDate;
            if (TryParseTime(fields[index], out candidateMinute))
            {
                parsedMinute = candidateMinute;
            }
            else if (TryParseDateTimeMinute(fields[index], out candidateDate, out candidateMinute))
            {
                if (candidateDate.Date != expectedDate.Date)
                {
                    throw InvalidRow(path, lineNumber, line);
                }
                parsedMinute = candidateMinute;
            }
        }
        if (parsedMinute >= 0)
        {
            currentMinute = parsedMinute;
        }
        else if (!(fields[0].Length == 0 && currentMinute >= 0))
        {
            throw InvalidRow(path, lineNumber, line);
        }
        appName = SanitizeField(appName);
    }

    private static InvalidDataException InvalidRow(string path, long lineNumber, string line)
    {
        string sample = line.Length <= 180 ? line : line.Substring(0, 180) + "...";
        return new InvalidDataException("Invalid history row in " + path + " at line " + lineNumber + ": " + sample);
    }

    private static bool TryParseDateTimeMinute(string text, out DateTime date, out int minute)
    {
        date = DateTime.MinValue;
        minute = -1;
        DateTime value;
        if (!DateTime.TryParseExact(text.Trim(), "yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture,
            DateTimeStyles.None, out value))
        {
            return false;
        }
        date = value.Date;
        minute = value.Hour * 60 + value.Minute;
        return true;
    }

    private static bool TryParseTime(string text, out int minute)
    {
        minute = -1;
        if (text == null || text.Length != 5 || text[2] != ':' ||
            text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9' ||
            text[3] < '0' || text[3] > '9' || text[4] < '0' || text[4] > '9')
        {
            return false;
        }
        int hour = (text[0] - '0') * 10 + text[1] - '0';
        int minutePart = (text[3] - '0') * 10 + text[4] - '0';
        if (hour > 23 || minutePart > 59)
        {
            return false;
        }
        minute = hour * 60 + minutePart;
        return true;
    }

    private static string FormatMinute(int minute)
    {
        return (minute / 60).ToString("D2", CultureInfo.InvariantCulture) + ":" +
            (minute % 60).ToString("D2", CultureInfo.InvariantCulture);
    }

    private static bool TryParseAmount(string text, out ulong value)
    {
        return ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);
    }

    private static ulong AddChecked(ulong left, ulong right)
    {
        if (ulong.MaxValue - left < right)
        {
            throw new OverflowException("Traffic total exceeds UInt64.");
        }
        return left + right;
    }

    private static bool IsAbnormal(ulong rx, ulong tx)
    {
        return rx > AbnormalMinuteThreshold || tx > AbnormalMinuteThreshold - rx;
    }

    private static string ToBase62(ulong value)
    {
        char[] buffer = new char[11];
        int index = buffer.Length;
        do
        {
            buffer[--index] = Base62Digits[(int)(value % 62UL)];
            value /= 62UL;
        }
        while (value != 0UL);
        return new string(buffer, index, buffer.Length - index);
    }

    private static string SanitizeField(string value)
    {
        return (value ?? string.Empty).Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ');
    }

    private static void AddSummary(SummaryData summary, ulong id, ulong rx, ulong tx)
    {
        Amount amount;
        if (!summary.ById.TryGetValue(id, out amount))
        {
            amount = new Amount();
            summary.ById.Add(id, amount);
        }
        amount.Rx = AddChecked(amount.Rx, rx);
        amount.Tx = AddChecked(amount.Tx, tx);
        summary.Rx = AddChecked(summary.Rx, rx);
        summary.Tx = AddChecked(summary.Tx, tx);
    }

    private static void MergeSummary(SummaryData target, SummaryData source)
    {
        foreach (KeyValuePair<ulong, Amount> entry in source.ById)
        {
            AddSummary(target, entry.Key, entry.Value.Rx, entry.Value.Tx);
        }
    }

    private static void WriteAppDictionary(string path, AppDictionary dictionary)
    {
        using (StreamWriter writer = OpenWriter(path))
        {
            writer.WriteLine("version\t" + LatestDictionaryVersion);
            foreach (KeyValuePair<ulong, string> entry in dictionary.ById)
            {
                writer.Write("app\t");
                writer.Write(ToBase62(entry.Key));
                writer.Write('\t');
                writer.WriteLine(SanitizeField(entry.Value));
            }
        }
    }

    private static SummaryCounts BuildSummaries(string historyDirectory, List<DayResult> days)
    {
        DateTime today = DateTime.Today;
        List<SummaryItem> dayItems = new List<SummaryItem>();
        SummaryCounts counts = new SummaryCounts();
        foreach (DayResult day in days)
        {
            if (day.Date >= today || day.OutputLines == 0)
            {
                continue;
            }
            WriteSummary(GetSummaryPath(historyDirectory, "day", day.Date), "day", day.Date, day.Date,
                (ulong)day.OutputLines, day.Generation, day.Summary);
            dayItems.Add(new SummaryItem { Start = day.Date, Summary = day.Summary, Generation = day.Generation });
            counts.Days++;
        }

        SortedDictionary<DateTime, List<SummaryItem>> weeks = new SortedDictionary<DateTime, List<SummaryItem>>();
        SortedDictionary<DateTime, List<SummaryItem>> months = new SortedDictionary<DateTime, List<SummaryItem>>();
        foreach (SummaryItem item in dayItems)
        {
            AddPeriodSource(weeks, GetWeekStart(item.Start), item);
            AddPeriodSource(months, new DateTime(item.Start.Year, item.Start.Month, 1), item);
        }

        foreach (KeyValuePair<DateTime, List<SummaryItem>> entry in weeks)
        {
            DateTime end = entry.Key.AddDays(6);
            if (end >= today) { continue; }
            SummaryItem combined = CombinePeriod(entry.Key, entry.Value);
            WriteSummary(GetSummaryPath(historyDirectory, "week", entry.Key), "week", entry.Key, end,
                (ulong)entry.Value.Count, combined.Generation, combined.Summary);
            counts.Weeks++;
        }

        List<SummaryItem> monthItems = new List<SummaryItem>();
        foreach (KeyValuePair<DateTime, List<SummaryItem>> entry in months)
        {
            DateTime end = entry.Key.AddMonths(1).AddDays(-1);
            if (end >= today) { continue; }
            SummaryItem combined = CombinePeriod(entry.Key, entry.Value);
            WriteSummary(GetSummaryPath(historyDirectory, "month", entry.Key), "month", entry.Key, end,
                (ulong)entry.Value.Count, combined.Generation, combined.Summary);
            monthItems.Add(combined);
            counts.Months++;
        }

        SortedDictionary<DateTime, List<SummaryItem>> quarters = new SortedDictionary<DateTime, List<SummaryItem>>();
        foreach (SummaryItem item in monthItems)
        {
            int firstMonth = ((item.Start.Month - 1) / 3) * 3 + 1;
            AddPeriodSource(quarters, new DateTime(item.Start.Year, firstMonth, 1), item);
        }
        foreach (KeyValuePair<DateTime, List<SummaryItem>> entry in quarters)
        {
            DateTime end = entry.Key.AddMonths(3).AddDays(-1);
            if (end >= today) { continue; }
            SummaryItem combined = CombinePeriod(entry.Key, entry.Value);
            WriteSummary(GetSummaryPath(historyDirectory, "quarter", entry.Key), "quarter", entry.Key, end,
                (ulong)entry.Value.Count, combined.Generation, combined.Summary);
            counts.Quarters++;
        }
        return counts;
    }

    private static void AddPeriodSource(
        SortedDictionary<DateTime, List<SummaryItem>> periods,
        DateTime start,
        SummaryItem item)
    {
        List<SummaryItem> sources;
        if (!periods.TryGetValue(start, out sources))
        {
            sources = new List<SummaryItem>();
            periods.Add(start, sources);
        }
        sources.Add(item);
    }

    private static SummaryItem CombinePeriod(DateTime start, List<SummaryItem> sources)
    {
        SummaryData summary = new SummaryData();
        ulong generation = 0;
        foreach (SummaryItem source in sources)
        {
            MergeSummary(summary, source.Summary);
            generation = unchecked(generation ^ (source.Generation + 0x9e3779b97f4a7c15UL +
                (generation << 6) + (generation >> 2)));
        }
        return new SummaryItem { Start = start, Summary = summary, Generation = generation };
    }

    private static void WriteSummary(
        string path,
        string type,
        DateTime start,
        DateTime end,
        ulong sourceCount,
        ulong sourceGeneration,
        SummaryData summary)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        using (StreamWriter writer = OpenWriter(path))
        {
            writer.WriteLine("version\t" + SummaryVersion);
            writer.WriteLine("type\t" + type);
            writer.WriteLine("start\t" + start.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture));
            writer.WriteLine("end\t" + end.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture));
            writer.WriteLine("complete\t1");
            writer.WriteLine("source_count\t" + sourceCount.ToString(CultureInfo.InvariantCulture));
            writer.WriteLine("source_generation\t" + sourceGeneration.ToString(CultureInfo.InvariantCulture));
            foreach (KeyValuePair<ulong, Amount> entry in summary.ById)
            {
                writer.Write("app\t");
                writer.Write(ToBase62(entry.Key));
                writer.Write('\t');
                writer.Write(ToBase62(entry.Value.Rx));
                writer.Write('\t');
                writer.WriteLine(ToBase62(entry.Value.Tx));
            }
            writer.Write("total\t");
            writer.Write(ToBase62(summary.Rx));
            writer.Write('\t');
            writer.WriteLine(ToBase62(summary.Tx));
        }
    }

    private static DateTime GetWeekStart(DateTime date)
    {
        int offset = ((int)date.DayOfWeek + 6) % 7;
        return date.Date.AddDays(-offset);
    }

    private static string GetDailyPath(string historyDirectory, DateTime date)
    {
        return Path.Combine(GetMonthDirectory(historyDirectory, date),
            date.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ".tsv");
    }

    private static string GetMonthDirectory(string historyDirectory, DateTime date)
    {
        return Path.Combine(historyDirectory,
            date.ToString("yyyy", CultureInfo.InvariantCulture),
            date.ToString("MM", CultureInfo.InvariantCulture));
    }

    private static string GetSummaryPath(string historyDirectory, string type, DateTime start)
    {
        if (type == "day")
        {
            return Path.Combine(GetMonthDirectory(historyDirectory, start),
                start.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ".day.tsv");
        }
        if (type == "month")
        {
            return Path.Combine(GetMonthDirectory(historyDirectory, start),
                start.ToString("yyyy-MM", CultureInfo.InvariantCulture) + ".month.tsv");
        }
        if (type == "quarter")
        {
            int quarter = ((start.Month - 1) / 3) + 1;
            return Path.Combine(historyDirectory, start.ToString("yyyy", CultureInfo.InvariantCulture),
                start.Year.ToString("D4", CultureInfo.InvariantCulture) + "-Q" + quarter + ".quarter.tsv");
        }
        DateTime thursday = start.AddDays(3);
        DateTime firstMonday = GetWeekStart(new DateTime(thursday.Year, 1, 4));
        int week = ((start - firstMonday).Days / 7) + 1;
        return Path.Combine(GetMonthDirectory(historyDirectory, start),
            thursday.Year.ToString("D4", CultureInfo.InvariantCulture) + "-W" +
            week.ToString("D2", CultureInfo.InvariantCulture) + ".week.tsv");
    }

    private static void ValidateOutput(string historyDirectory, int expectedDays, int expectedApps)
    {
        string dictionaryPath = Path.Combine(historyDirectory, "apps.tsv");
        if (!File.Exists(dictionaryPath))
        {
            throw new InvalidDataException("Output validation failed: apps.tsv is missing.");
        }
        int apps = -1;
        using (StreamReader reader = OpenReader(dictionaryPath))
        {
            if (!string.Equals(reader.ReadLine(), "version\t" + LatestDictionaryVersion, StringComparison.Ordinal))
            {
                throw new InvalidDataException("Output validation failed: invalid apps.tsv version.");
            }
            apps = 0;
            while (reader.ReadLine() != null) { apps++; }
        }
        int days = 0;
        foreach (string path in Directory.GetFiles(historyDirectory, "*.tsv", SearchOption.AllDirectories))
        {
            if (DailyName.IsMatch(Path.GetFileName(path))) { days++; }
        }
        if (days != expectedDays || apps != expectedApps)
        {
            throw new InvalidDataException("Output validation failed: converted file or application count does not match.");
        }
    }

    private static string Activate(
        string baseDirectory,
        string historyDirectory,
        string legacySingle,
        string statePath,
        string stagingDirectory,
        ulong allRx,
        ulong allTx)
    {
        string backupPath = Path.Combine(baseDirectory,
            ".traffic_history_migration_" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture));
        Directory.CreateDirectory(backupPath);
        string backupHistory = Path.Combine(backupPath, "app_traffic_history");
        string backupSingle = Path.Combine(backupPath, "app_traffic_history.tsv");
        string backupState = Path.Combine(backupPath, "app_traffic_state.tsv");
        bool hadHistory = Directory.Exists(historyDirectory);
        bool hadSingle = File.Exists(legacySingle);
        bool hadState = File.Exists(statePath);
        bool historyMoved = false;
        bool singleMoved = false;
        bool newHistoryActivated = false;

        try
        {
            if (hadState) { File.Copy(statePath, backupState, true); }
            if (hadHistory)
            {
                Directory.Move(historyDirectory, backupHistory);
                historyMoved = true;
            }
            if (hadSingle)
            {
                File.Move(legacySingle, backupSingle);
                singleMoved = true;
            }
            Directory.Move(stagingDirectory, historyDirectory);
            newHistoryActivated = true;
            WriteLatestState(statePath, allRx, allTx, hadState ? backupState : null);
        }
        catch
        {
            if (newHistoryActivated && Directory.Exists(historyDirectory))
            {
                string failedPath = Path.Combine(baseDirectory, ".traffic_history_failed_" + Guid.NewGuid().ToString("N"));
                Directory.Move(historyDirectory, failedPath);
            }
            if (historyMoved && Directory.Exists(backupHistory)) { CopyDirectory(backupHistory, historyDirectory); }
            if (singleMoved && File.Exists(backupSingle)) { File.Copy(backupSingle, legacySingle, true); }
            if (hadState)
            {
                if (File.Exists(backupState)) { File.Copy(backupState, statePath, true); }
            }
            else if (File.Exists(statePath))
            {
                File.Delete(statePath);
            }
            throw;
        }
        return backupPath;
    }

    private static void WriteLatestState(string path, ulong allRx, ulong allTx, string previousState)
    {
        List<string> preserved = new List<string>();
        if (!string.IsNullOrEmpty(previousState) && File.Exists(previousState))
        {
            foreach (string line in File.ReadLines(previousState, Utf8))
            {
                int separator = line.IndexOf('\t');
                if (separator <= 0) { continue; }
                string key = line.Substring(0, separator);
                if (key == "language" || key == "range_start" || key == "range_end" || key == "path")
                {
                    preserved.Add(line.Replace('\r', ' ').Replace('\n', ' '));
                }
            }
        }
        string temporary = path + ".new";
        using (StreamWriter writer = OpenWriter(temporary))
        {
            writer.WriteLine("history_format\t" + LatestHistoryFormat);
            writer.WriteLine("history_archive_format\t" + LatestArchiveFormat);
            writer.WriteLine("history_migration_format\t" + LatestMigrationFormat);
            foreach (string line in preserved) { writer.WriteLine(line); }
            writer.WriteLine("all_time\t" + allRx.ToString(CultureInfo.InvariantCulture) + "\t" +
                allTx.ToString(CultureInfo.InvariantCulture));
        }
        if (File.Exists(path))
        {
            string replaceBackup = path + ".replace.bak";
            if (File.Exists(replaceBackup)) { File.Delete(replaceBackup); }
            File.Replace(temporary, path, replaceBackup);
            if (File.Exists(replaceBackup)) { File.Delete(replaceBackup); }
        }
        else
        {
            File.Move(temporary, path);
        }
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (string file in Directory.GetFiles(source))
        {
            File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), true);
        }
        foreach (string directory in Directory.GetDirectories(source))
        {
            CopyDirectory(directory, Path.Combine(destination, Path.GetFileName(directory)));
        }
    }

    private static StreamReader OpenReader(string path)
    {
        return new StreamReader(path, Utf8, true, 65536);
    }

    private static StreamWriter OpenWriter(string path)
    {
        string parent = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(parent)) { Directory.CreateDirectory(parent); }
        StreamWriter writer = new StreamWriter(path, false, Utf8, 65536);
        writer.NewLine = "\n";
        return writer;
    }
}

'@
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Find-TrafficMonitorDirectory {
    if (-not [string]::IsNullOrWhiteSpace($env:TM_HISTORY_BASEDIR)) {
        return (Resolve-Path -LiteralPath $env:TM_HISTORY_BASEDIR).Path
    }

    $candidate = if (-not [string]::IsNullOrWhiteSpace($env:TM_HISTORY_TOOL)) {
        Split-Path -Parent $env:TM_HISTORY_TOOL
    }
    else {
        $PSScriptRoot
    }
    $historyCandidate = $null
    while (-not [string]::IsNullOrWhiteSpace($candidate)) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'TrafficMonitor.exe') -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        if ($null -eq $historyCandidate -and
            ((Test-Path -LiteralPath (Join-Path $candidate 'app_traffic_history') -PathType Container) -or
             (Test-Path -LiteralPath (Join-Path $candidate 'app_traffic_history.tsv') -PathType Leaf))) {
            $historyCandidate = (Resolve-Path -LiteralPath $candidate).Path
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $candidate) { break }
        $candidate = $parent
    }
    if ($null -ne $historyCandidate) { return $historyCandidate }
    throw 'TrafficMonitor.exe or a published traffic-history layout was not found above the tools directory.'
}

try {
    $baseDirectory = Find-TrafficMonitorDirectory
    if (-not (Get-Variable -Name embeddedMigrationSource -Scope Script -ErrorAction SilentlyContinue)) {
        $sourcePath = Join-Path $PSScriptRoot 'UpdateTrafficHistory.cs'
        $script:embeddedMigrationSource = [System.IO.File]::ReadAllText($sourcePath)
    }
    Add-Type -TypeDefinition $script:embeddedMigrationSource -Language CSharp
    $allowRunning = $env:TM_HISTORY_ALLOW_RUNNING -eq '1'
    exit [TrafficHistoryMigration]::Run($baseDirectory, $allowRunning)
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'The original history was not changed, or was restored if activation had started.'
    exit 1
}
