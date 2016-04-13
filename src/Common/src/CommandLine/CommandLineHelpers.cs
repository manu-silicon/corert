// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Collections.Generic;
using System.IO;

namespace Internal.CommandLine
{
    //
    // Helpers for command line processing
    //
    internal class Helpers
    {
        // Helper to create a collection of paths unique in their simple names.
        public static void AppendExpandedPaths(Dictionary<string, List<string>> dictionary, string pattern, bool strict)
        {
            bool empty = true;

            string directoryName = Path.GetDirectoryName(pattern);
            string searchPattern = Path.GetFileName(pattern);

            if (directoryName == "")
                directoryName = ".";

            if (Directory.Exists(directoryName))
            {
                foreach (string fileName in Directory.EnumerateFiles(directoryName, searchPattern))
                {
                    string fullFileName = Path.GetFullPath(fileName);
                    string simpleName = Path.GetFileNameWithoutExtension(fileName);

                    // Lookup an existing assembly
                    List<string> list = null;
                    if (!dictionary.TryGetValue(simpleName, out list))
                    {
                        // Normal case
                        list = new List<string>(1);
                        list.Add(fullFileName);
                        dictionary.Add(simpleName, list);
                    }
                    else
                    {
                        if (list.Contains(fullFileName))
                        {
                            if (strict)
                            {
                                throw new CommandLineException("Repeated input files " + fullFileName);
                            }
                        }
                        else
                        {
                            list.Add(fullFileName);
                        }
                    }
                    empty = false;
                }
            }

            if (empty)
            {
                if (strict)
                {
                    throw new CommandLineException("No files matching " + pattern);
                }
                else
                {
                    Console.WriteLine("Warning: No files matching " + pattern);
                }
            }
        }
    }
}
