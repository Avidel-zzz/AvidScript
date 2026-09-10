using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32.SafeHandles;

namespace AvidScript.Release.BinaryPrivacyV1
{
    public sealed class Rule
    {
        public string Id { get; }
        public int WindowChars { get; }
        private readonly string literal;
        private readonly Regex expression;

        public Rule(string id, string pattern, bool regex, int windowChars)
        {
            if (!Regex.IsMatch(id ?? "", "^[a-z][a-z0-9_-]{0,63}$") ||
                string.IsNullOrEmpty(pattern) || pattern.Length > 4096 ||
                windowChars < 1 || windowChars > 4096 || (!regex && windowChars < pattern.Length))
                throw new ArgumentException("ASBP1001 invalid scan rule");
            Id = id;
            WindowChars = windowChars;
            if (regex)
                expression = new Regex(pattern, RegexOptions.IgnoreCase | RegexOptions.CultureInvariant,
                    TimeSpan.FromMilliseconds(250));
            else
                literal = pattern;
        }

        public bool Matches(string text) => expression != null
            ? expression.IsMatch(text)
            : text.IndexOf(literal, StringComparison.OrdinalIgnoreCase) >= 0;
    }

    public sealed class FileResult
    {
        public long Length { get; set; }
        public string Sha256 { get; set; }
        // Counts positive decoding windows, not distinct occurrences in the original byte stream.
        public Dictionary<string, long> MatchedViews { get; } = new Dictionary<string, long>(StringComparer.Ordinal);
    }

    public static class Scanner
    {
        private static string sourceIdentity;
        public static void BindSourceIdentity(string sha256)
        {
            if (sourceIdentity != null && !string.Equals(sourceIdentity, sha256, StringComparison.Ordinal))
                throw new InvalidOperationException("ASBP1001 loaded scanner source changed; start a fresh process");
            sourceIdentity = sha256;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandleW(
            SafeFileHandle file, StringBuilder path, uint capacity, uint flags);

        // Use the actual open handle, so a link substituted between enumeration and open cannot escape the root.
        // https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew
        public static void AssertHandlePath(FileStream stream, string expectedPath)
        {
            var buffer = new StringBuilder(32768);
            uint length = GetFinalPathNameByHandleW(stream.SafeFileHandle, buffer, (uint)buffer.Capacity, 0);
            if (length == 0 || length >= buffer.Capacity)
                throw new IOException("ASBP1004 cannot verify open file path");
            string actual = buffer.ToString();
            if (actual.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
                actual = @"\\" + actual.Substring(8);
            else if (actual.StartsWith(@"\\?\", StringComparison.Ordinal))
                actual = actual.Substring(4);
            if (!string.Equals(actual, Path.GetFullPath(expectedPath).Replace('/', '\\'), StringComparison.OrdinalIgnoreCase))
                throw new IOException("ASBP1004 open file path differs from inventory");
        }

        public static string[] MatchName(string name, Rule[] rules) =>
            rules.Where(rule => rule.Matches(name)).Select(rule => rule.Id).ToArray();

        public static FileResult ScanStream(FileStream stream, Rule[] rules, int blockSize)
        {
            if (rules == null || rules.Length == 0 || rules.Length > 256 || blockSize < 64 || blockSize > 1048576 ||
                !stream.CanRead || !stream.CanSeek || stream.Position != 0)
                throw new ArgumentException("ASBP1001 invalid scan stream or limits");
            int overlap = checked(rules.Max(rule => rule.WindowChars) * 4 + 64);
            byte[] buffer = new byte[checked(blockSize + overlap)];
            int retained = 0;
            var result = new FileResult();
            using (var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                int read;
                while ((read = stream.Read(buffer, retained, blockSize)) != 0)
                {
                    hash.AppendData(buffer, retained, read);
                    result.Length = checked(result.Length + read);
                    int available = retained + read;
                    MatchView(Encoding.UTF8.GetString(buffer, 0, available), rules, result);
                    // Binary strings can begin at either byte parity, regardless of file or chunk alignment.
                    foreach (int parity in new[] { 0, 1 })
                    {
                        int count = (available - parity) & ~1;
                        if (count > 0)
                        {
                            MatchView(Encoding.Unicode.GetString(buffer, parity, count), rules, result);
                            MatchView(Encoding.BigEndianUnicode.GetString(buffer, parity, count), rules, result);
                        }
                    }
                    retained = Math.Min(overlap, available);
                    Buffer.BlockCopy(buffer, available - retained, buffer, 0, retained);
                }
                result.Sha256 = Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
            }
            return result;
        }

        private static void MatchView(string view, Rule[] rules, FileResult result)
        {
            foreach (Rule rule in rules)
            {
                if (!rule.Matches(view)) continue;
                result.MatchedViews.TryGetValue(rule.Id, out long count);
                result.MatchedViews[rule.Id] = checked(count + 1);
            }
        }
    }
}
