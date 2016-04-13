// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Diagnostics;
using System.Collections.Generic;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;

using ILCompiler.SymbolReader;

using Internal.TypeSystem;
using Internal.TypeSystem.Ecma;
using Internal.IL;

namespace ILCompiler
{
    public class CompilerTypeSystemContext : MetadataTypeSystemContext, IMetadataStringDecoderProvider
    {
        private MetadataFieldLayoutAlgorithm _metadataFieldLayoutAlgorithm = new CompilerMetadataFieldLayoutAlgorithm();
        private MetadataRuntimeInterfacesAlgorithm _metadataRuntimeInterfacesAlgorithm = new MetadataRuntimeInterfacesAlgorithm();
        private ArrayOfTRuntimeInterfacesAlgorithm _arrayOfTRuntimeInterfacesAlgorithm;
        private MetadataVirtualMethodAlgorithm _virtualMethodAlgorithm = new MetadataVirtualMethodAlgorithm();
        private MetadataVirtualMethodEnumerationAlgorithm _virtualMethodEnumAlgorithm = new MetadataVirtualMethodEnumerationAlgorithm();
        private DelegateVirtualMethodEnumerationAlgorithm _delegateVirtualMethodEnumAlgorithm = new DelegateVirtualMethodEnumerationAlgorithm();

        private MetadataStringDecoder _metadataStringDecoder;

        private class ModuleData
        {
            public string SimpleName;
            public string FilePath;

            public EcmaModule Module;
            public MemoryMappedViewAccessor MappedViewAccessor;

            public PdbSymbolReader PdbReader;
        }

        private class ModuleHashtable : LockFreeReaderHashtable<EcmaModule, ModuleData>
        {
            protected override int GetKeyHashCode(EcmaModule key)
            {
                return key.GetHashCode();
            }
            protected override int GetValueHashCode(ModuleData value)
            {
                return value.Module.GetHashCode();
            }
            protected override bool CompareKeyToValue(EcmaModule key, ModuleData value)
            {
                return Object.ReferenceEquals(key, value.Module);
            }
            protected override bool CompareValueToValue(ModuleData value1, ModuleData value2)
            {
                return Object.ReferenceEquals(value1.Module, value2.Module);
            }
            protected override ModuleData CreateValueFromKey(EcmaModule key)
            {
                Debug.Assert(false, "CreateValueFromKey not supported");
                return null;
            }
        }
        private ModuleHashtable _moduleHashtable = new ModuleHashtable();

        /// <summary>
        /// Mapping between simple names and a list of modules sharing that simple name.
        /// </summary>
        private class SimpleNameHashtable : LockFreeReaderHashtable<string, List<ModuleData>>
        {
            StringComparer _comparer = StringComparer.OrdinalIgnoreCase;

            protected override int GetKeyHashCode(string key)
            {
                return _comparer.GetHashCode(key);
            }
            protected override int GetValueHashCode(List<ModuleData> value)
            {
                return _comparer.GetHashCode(value[0].SimpleName);
            }
            protected override bool CompareKeyToValue(string key, List<ModuleData> value)
            {
                return _comparer.Equals(key, value[0].SimpleName);
            }
            protected override bool CompareValueToValue(List<ModuleData> value1, List<ModuleData> value2)
            {
                return _comparer.Equals(value1[0].SimpleName, value2[0].SimpleName);
            }
            protected override List<ModuleData> CreateValueFromKey(string key)
            {
                Debug.Assert(false, "CreateValueFromKey not supported");
                return null;
            }
        }
        private SimpleNameHashtable _simpleNameHashtable = new SimpleNameHashtable();

        private class DelegateInfoHashtable : LockFreeReaderHashtable<TypeDesc, DelegateInfo>
        {
            protected override int GetKeyHashCode(TypeDesc key)
            {
                return key.GetHashCode();
            }
            protected override int GetValueHashCode(DelegateInfo value)
            {
                return value.Type.GetHashCode();
            }
            protected override bool CompareKeyToValue(TypeDesc key, DelegateInfo value)
            {
                return Object.ReferenceEquals(key, value.Type);
            }
            protected override bool CompareValueToValue(DelegateInfo value1, DelegateInfo value2)
            {
                return Object.ReferenceEquals(value1.Type, value2.Type);
            }
            protected override DelegateInfo CreateValueFromKey(TypeDesc key)
            {
                return new DelegateInfo(key);
            }
        }
        private DelegateInfoHashtable _delegateInfoHashtable = new DelegateInfoHashtable();

        public CompilerTypeSystemContext(TargetDetails details)
            : base(details)
        {
        }

        /// <summary>
        /// List of assemblies to process.
        /// Associate for each simple name, the list of assembly full path names matching the simple name.
        /// Useful when including multiple assemblies with same name from different locations.
        /// </summary>
        public IReadOnlyDictionary<string, List<string>> InputFilePaths
        {
            get;
            set;
        }

        /// <summary>
        /// List of assemblies referenced by assemblies to process stored in <see cref="InputFilePaths"/>.
        /// Associate for each simple name, the list of assembly full path names matching the simple name.
        /// Useful when including multiple assemblies with same name from different locations.
        /// </summary>
        public IReadOnlyDictionary<string, List<string>> ReferenceFilePaths
        {
            get;
            set;
        }

        public override ModuleDesc ResolveAssembly(System.Reflection.AssemblyName name, bool throwIfNotFoundOrAmbiguous = true)
        {
            var list = GetModulesForSimpleName(name.Name, throwIfNotFoundOrAmbiguous);
            int n = 0;
            EcmaModule result = null;
            if (list != null)
            {
                if (list.Count == 1)
                {
                    result = list[0].Module;
                    if (throwIfNotFoundOrAmbiguous && !IsAssemblyNameCompatible(result.GetName(), name))
                    {
                        throw new NotSupportedException("Non-matching assembly found. Expected " + name.FullName + " but got " + result.GetName().FullName);
                    }
                    return result;
                }
                else
                {
                    foreach (var module in list)
                    {
                        if (IsAssemblyNameCompatible(module.Module.GetName(), name))
                        {
                            result = module.Module;
                            n++;
                        }
                    }
                }
            }
            if ((n == 0) && throwIfNotFoundOrAmbiguous)
            {
                throw new FileNotFoundException("Assembly not found: " + name.FullName);
            }
            if ((n > 1) && throwIfNotFoundOrAmbiguous)
            {
                // TODO: Ideally this should not happen and the command line should only include
                // TODO: one of them, not more. For now, we do nothing and return the last found.
                // throw new NotImplementedException(
                //    "Two or more assemblies with the same FullName but different paths: " + name.FullName);
            }
            return result;
        }

        /// <summary>
        /// Is assembly <param name="source"/> compatible with <param name="target"/>? For now we compare
        /// that they have the same name, <param name="source"/> has a greater version than expected,
        /// and if <param name="target"/> is signed that <param name="source"/> has the same public key.
        /// </summary>
        /// <param name="source">Source assembly</param>
        /// <param name="target">Expected assembly</param>
        /// <returns>True if <param name="source"/> is a good replacement for <param name="target"/></returns>
        private static bool IsAssemblyNameCompatible(System.Reflection.AssemblyName source, System.Reflection.AssemblyName target)
        {
            if (source.Name == target.Name)
            {
                bool isSame = true;
                if (target.Version != null)
                {
                    // Ensure that source has a version greater than target.
                    isSame = (source.Version != null) && (source.Version >= target.Version);
                }
                if (isSame)
                {
                    // Compare the public key token
                    var targetToken = target.GetPublicKeyToken();
                    if (targetToken != null)
                    {
                        var sourceToken = source.GetPublicKeyToken();
                        if (sourceToken == targetToken)
                            return true;

                        if (sourceToken == null)
                            return false;

                        if (sourceToken.Length != targetToken.Length)
                            return false;

                        for (int i = 0; i < sourceToken.Length; i++)
                        {
                            if (sourceToken[i] != targetToken[i])
                                return false;
                        }
                        return true;
                    }
                    else
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        public ModuleDesc GetModuleForSimpleName(string simpleName, bool throwIfNotFoundOrAmbiguous = true)
        {
            var name = new System.Reflection.AssemblyName(simpleName);
            return ResolveAssembly(name, throwIfNotFoundOrAmbiguous);
        }

        private List<ModuleData> GetModulesForSimpleName(string simpleName, bool throwIfNotFoundOrAmbiguous = true)
        {
            List<ModuleData> existing;
            if (_simpleNameHashtable.TryGetValue(simpleName, out existing))
                return existing;

            List<string> listFilePath;
            if (!InputFilePaths.TryGetValue(simpleName, out listFilePath))
            {
                if (!ReferenceFilePaths.TryGetValue(simpleName, out listFilePath))
                {
                    if (throwIfNotFoundOrAmbiguous)
                        throw new FileNotFoundException("Assembly not found: " + simpleName);
                    return null;
                }
            }

            var moduleList = new List<ModuleData>(listFilePath.Count);
            foreach (var filePath in listFilePath)
            {
                moduleList.Add(AddModule(filePath, simpleName));
            }
            return moduleList;
        }

        public EcmaModule GetModuleFromPath(string filePath)
        {
            // This method is not expected to be called frequently. Linear search is acceptable.
            foreach (var entry in ModuleHashtable.Enumerator.Get(_moduleHashtable))
            {
                if (entry.FilePath == filePath)
                    return entry.Module;
            }

            return AddModule(filePath, null).Module;
        }

        private unsafe static PEReader OpenPEFile(string filePath, out MemoryMappedViewAccessor mappedViewAccessor)
        {
            // System.Reflection.Metadata has heuristic that tries to save virtual address space. This heuristic does not work
            // well for us since it can make IL access very slow (call to OS for each method IL query). We will map the file
            // ourselves to get the desired performance characteristics reliably.

            FileStream fileStream = null;
            MemoryMappedFile mappedFile = null;
            MemoryMappedViewAccessor accessor = null;
            try
            {
                // Create stream because CreateFromFile(string, ...) uses FileShare.None which is too strict
                fileStream = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, false);
                mappedFile = MemoryMappedFile.CreateFromFile(
                    fileStream, null, fileStream.Length, MemoryMappedFileAccess.Read, HandleInheritability.None, true);
                accessor = mappedFile.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);

                var safeBuffer = accessor.SafeMemoryMappedViewHandle;
                var peReader = new PEReader((byte*)safeBuffer.DangerousGetHandle(), (int)safeBuffer.ByteLength);

                // MemoryMappedFile does not need to be kept around. MemoryMappedViewAccessor is enough.

                mappedViewAccessor = accessor;
                accessor = null;

                return peReader;
            }
            finally
            {
                if (accessor != null)
                    accessor.Dispose();
                if (mappedFile != null)
                    mappedFile.Dispose();
                if (fileStream != null)
                    fileStream.Dispose();
            }
        }

        private ModuleData AddModule(string filePath, string expectedSimpleName)
        {
            MemoryMappedViewAccessor mappedViewAccessor = null;
            try
            {
                PEReader peReader = OpenPEFile(filePath, out mappedViewAccessor);

                EcmaModule module = new EcmaModule(this, peReader);

                MetadataReader metadataReader = module.MetadataReader;
                string simpleName = metadataReader.GetString(metadataReader.GetAssemblyDefinition().Name);

                if (expectedSimpleName != null && !simpleName.Equals(expectedSimpleName, StringComparison.OrdinalIgnoreCase))
                    throw new FileNotFoundException("Assembly name does not match filename " + filePath);

                ModuleData moduleData = new ModuleData()
                {
                    SimpleName = simpleName,
                    FilePath = filePath,
                    Module = module,
                    MappedViewAccessor = mappedViewAccessor
                };
                var list = new List<ModuleData>(1);
                list.Add(moduleData);

                lock (this)
                {
                    List<ModuleData> actualModuleDataList = _simpleNameHashtable.AddOrGetExisting(list);
                    if (actualModuleDataList != list)
                    {
                        // Insert moduleData in list if not already present.
                        ModuleData foundModuleData = null;
                        for (int i = 0, nb = actualModuleDataList.Count; (foundModuleData == null) && (i < nb); i++)
                        {
                            if (actualModuleDataList[i].FilePath == filePath)
                            {
                                foundModuleData = actualModuleDataList[i];
                            }
                        }
                        if (foundModuleData != null)
                        {
                            // Module was previously found, we can return the computed value immediately
                            return foundModuleData;
                        }
                        else
                        {
                            actualModuleDataList.Add(moduleData);
                        }
                    }
                    mappedViewAccessor = null; // Ownership has been transfered

                    _moduleHashtable.AddOrGetExisting(moduleData);
                }

                // TODO: Thread-safety for symbol reading
                InitializeSymbolReader(moduleData);

                return moduleData;
            }
            finally
            {
                if (mappedViewAccessor != null)
                    mappedViewAccessor.Dispose();
            }
        }

        public DelegateInfo GetDelegateInfo(TypeDesc delegateType)
        {
            return _delegateInfoHashtable.GetOrCreateValue(delegateType);
        }

        public override FieldLayoutAlgorithm GetLayoutAlgorithmForType(DefType type)
        {
            return _metadataFieldLayoutAlgorithm;
        }

        protected override RuntimeInterfacesAlgorithm GetRuntimeInterfacesAlgorithmForNonPointerArrayType(ArrayType type)
        {
            if (_arrayOfTRuntimeInterfacesAlgorithm == null)
            {
                _arrayOfTRuntimeInterfacesAlgorithm = new ArrayOfTRuntimeInterfacesAlgorithm(SystemModule.GetKnownType("System", "Array`1"));
            }
            return _arrayOfTRuntimeInterfacesAlgorithm;
        }

        protected override RuntimeInterfacesAlgorithm GetRuntimeInterfacesAlgorithmForDefType(DefType type)
        {
            return _metadataRuntimeInterfacesAlgorithm;
        }

        public override VirtualMethodAlgorithm GetVirtualMethodAlgorithmForType(TypeDesc type)
        {
            Debug.Assert(!type.IsArray, "Wanted to call GetClosestMetadataType?");

            return _virtualMethodAlgorithm;
        }

        public override VirtualMethodEnumerationAlgorithm GetVirtualMethodEnumerationAlgorithmForType(TypeDesc type)
        {
            Debug.Assert(!type.IsArray, "Wanted to call GetClosestMetadataType?");

            if (type.IsDelegate)
                return _delegateVirtualMethodEnumAlgorithm;

            return _virtualMethodEnumAlgorithm;
        }

        public MetadataStringDecoder GetMetadataStringDecoder()
        {
            if (_metadataStringDecoder == null)
                _metadataStringDecoder = new CachingMetadataStringDecoder(0x10000); // TODO: Tune the size
            return _metadataStringDecoder;
        }

        //
        // Symbols
        //

        private void InitializeSymbolReader(ModuleData moduleData)
        {
            // Assume that the .pdb file is next to the binary
            var pdbFilename = Path.ChangeExtension(moduleData.FilePath, ".pdb");

            if (!File.Exists(pdbFilename))
                return;

            // Try to open the symbol file as portable pdb first
            PdbSymbolReader reader = PortablePdbSymbolReader.TryOpen(pdbFilename, GetMetadataStringDecoder());
            if (reader == null)
            {
                // Fallback to the diasymreader for non-portable pdbs
                reader = UnmanagedPdbSymbolReader.TryOpenSymbolReaderForMetadataFile(moduleData.FilePath);
            }

            moduleData.PdbReader = reader;
        }

        public IEnumerable<ILSequencePoint> GetSequencePointsForMethod(MethodDesc method)
        {
            EcmaMethod ecmaMethod = method.GetTypicalMethodDefinition() as EcmaMethod;
            if (ecmaMethod == null)
                return null;

            ModuleData moduleData;
            _moduleHashtable.TryGetValue(ecmaMethod.Module, out moduleData);
            Debug.Assert(moduleData != null);

            if (moduleData.PdbReader == null)
                return null;

            return moduleData.PdbReader.GetSequencePointsForMethod(MetadataTokens.GetToken(ecmaMethod.Handle));
        }

        public IEnumerable<ILLocalVariable> GetLocalVariableNamesForMethod(MethodDesc method)
        {
            EcmaMethod ecmaMethod = method.GetTypicalMethodDefinition() as EcmaMethod;
            if (ecmaMethod == null)
                return null;

            ModuleData moduleData;
            _moduleHashtable.TryGetValue(ecmaMethod.Module, out moduleData);
            Debug.Assert(moduleData != null);

            if (moduleData.PdbReader == null)
                return null;

            return moduleData.PdbReader.GetLocalVariableNamesForMethod(MetadataTokens.GetToken(ecmaMethod.Handle));
        }

        public IEnumerable<string> GetParameterNamesForMethod(MethodDesc method)
        {
            EcmaMethod ecmaMethod = method.GetTypicalMethodDefinition() as EcmaMethod;
            if (ecmaMethod == null)
                yield break;

            ParameterHandleCollection parameters = ecmaMethod.MetadataReader.GetMethodDefinition(ecmaMethod.Handle).GetParameters();

            if (!ecmaMethod.Signature.IsStatic)
            {
                yield return "_this";
            }

            foreach (var parameterHandle in parameters)
            {
                Parameter p = ecmaMethod.MetadataReader.GetParameter(parameterHandle);
                yield return ecmaMethod.MetadataReader.GetString(p.Name);
            }
        }
    }
}
