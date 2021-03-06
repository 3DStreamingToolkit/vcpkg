#include "pch.h"

#if defined(_WIN32)

#include <vcpkg/base/sortedvector.h>
#include <vcpkg/base/stringrange.h>
#include <vcpkg/base/util.h>
#include <vcpkg/visualstudio.h>

namespace vcpkg::VisualStudio
{
    static constexpr CStringView V_120 = "v120";
    static constexpr CStringView V_140 = "v140";
    static constexpr CStringView V_141 = "v141";

    struct VisualStudioInstance
    {
        enum class ReleaseType
        {
            STABLE,
            PRERELEASE,
            LEGACY
        };

        static bool preferred_first_comparator(const VisualStudioInstance& left, const VisualStudioInstance& right)
        {
            const auto get_preference_weight = [](const ReleaseType& type) -> int {
                switch (type)
                {
                    case ReleaseType::STABLE: return 3;
                    case ReleaseType::PRERELEASE: return 2;
                    case ReleaseType::LEGACY: return 1;
                    default: Checks::unreachable(VCPKG_LINE_INFO);
                }
            };

            if (left.release_type != right.release_type)
            {
                return get_preference_weight(left.release_type) > get_preference_weight(right.release_type);
            }

            return left.version > right.version;
        }

        VisualStudioInstance(fs::path&& root_path, std::string&& version, const ReleaseType& release_type)
            : root_path(std::move(root_path)), version(std::move(version)), release_type(release_type)
        {
        }

        fs::path root_path;
        std::string version;
        ReleaseType release_type;

        std::string major_version() const { return version.substr(0, 2); }
    };

    static std::vector<VisualStudioInstance> get_visual_studio_instances(const VcpkgPaths& paths)
    {
        const auto& fs = paths.get_filesystem();
        std::vector<VisualStudioInstance> instances;

        const auto& program_files_32_bit = System::get_program_files_32_bit().value_or_exit(VCPKG_LINE_INFO);

        // Instances from vswhere
        const fs::path vswhere_exe = program_files_32_bit / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
        if (fs.exists(vswhere_exe))
        {
            const auto code_and_output = System::cmd_execute_and_capture_output(
                Strings::format(R"("%s" -prerelease -legacy -products * -format xml)", vswhere_exe.u8string()));
            Checks::check_exit(VCPKG_LINE_INFO,
                               code_and_output.exit_code == 0,
                               "Running vswhere.exe failed with message:\n%s",
                               code_and_output.output);

            const auto instance_entries =
                StringRange::find_all_enclosed(code_and_output.output, "<instance>", "</instance>");
            for (const StringRange& instance : instance_entries)
            {
                auto maybe_is_prerelease =
                    StringRange::find_at_most_one_enclosed(instance, "<isPrerelease>", "</isPrerelease>");

                VisualStudioInstance::ReleaseType release_type = VisualStudioInstance::ReleaseType::LEGACY;
                if (const auto p = maybe_is_prerelease.get())
                {
                    const auto s = p->to_string();
                    if (s == "0")
                        release_type = VisualStudioInstance::ReleaseType::STABLE;
                    else if (s == "1")
                        release_type = VisualStudioInstance::ReleaseType::PRERELEASE;
                    else
                        Checks::unreachable(VCPKG_LINE_INFO);
                }

                instances.emplace_back(
                    StringRange::find_exactly_one_enclosed(instance, "<installationPath>", "</installationPath>")
                        .to_string(),
                    StringRange::find_exactly_one_enclosed(instance, "<installationVersion>", "</installationVersion>")
                        .to_string(),
                    release_type);
            }
        }

        const auto append_if_has_cl = [&](fs::path&& path_root) {
            const auto cl_exe = path_root / "VC" / "bin" / "cl.exe";
            const auto vcvarsall_bat = path_root / "VC" / "vcvarsall.bat";

            if (fs.exists(cl_exe) && fs.exists(vcvarsall_bat))
                instances.emplace_back(std::move(path_root), "14.0", VisualStudioInstance::ReleaseType::LEGACY);
        };

        // VS2015 instance from environment variable
        auto maybe_vs140_comntools = System::get_environment_variable("vs140comntools");
        if (const auto path_as_string = maybe_vs140_comntools.get())
        {
            // We want lexically_normal(), but it is not available
            // Correct root path might be 2 or 3 levels up, depending on if the path has trailing backslash. Try both.
            auto common7_tools = fs::path{*path_as_string};
            append_if_has_cl(fs::path{*path_as_string}.parent_path().parent_path());
            append_if_has_cl(fs::path{*path_as_string}.parent_path().parent_path().parent_path());
        }

        // VS2015 instance from Program Files
        append_if_has_cl(program_files_32_bit / "Microsoft Visual Studio 14.0");

        return instances;
    }

    std::vector<Toolset> find_toolset_instances_preferred_first(const VcpkgPaths& paths)
    {
        using CPU = System::CPUArchitecture;

        const auto& fs = paths.get_filesystem();

        // Note: this will contain a mix of vcvarsall.bat locations and dumpbin.exe locations.
        std::vector<fs::path> paths_examined;

        std::vector<Toolset> found_toolsets;
        std::vector<Toolset> excluded_toolsets;

        const SortedVector<VisualStudioInstance> sorted{get_visual_studio_instances(paths),
                                                        VisualStudioInstance::preferred_first_comparator};

        const bool v140_is_available = Util::find_if(sorted, [&](const VisualStudioInstance& vs_instance) {
                                           return vs_instance.major_version() == "14";
                                       }) != sorted.end();

        for (const VisualStudioInstance& vs_instance : sorted)
        {
            const std::string major_version = vs_instance.major_version();
            if (major_version == "15")
            {
                const fs::path vc_dir = vs_instance.root_path / "VC";

                // Skip any instances that do not have vcvarsall.
                const fs::path vcvarsall_dir = vc_dir / "Auxiliary" / "Build";
                const fs::path vcvarsall_bat = vcvarsall_dir / "vcvarsall.bat";
                paths_examined.push_back(vcvarsall_bat);
                if (!fs.exists(vcvarsall_bat)) continue;

                // Get all supported architectures
                std::vector<ToolsetArchOption> supported_architectures;
                if (fs.exists(vcvarsall_dir / "vcvars32.bat"))
                    supported_architectures.push_back({"x86", CPU::X86, CPU::X86});
                if (fs.exists(vcvarsall_dir / "vcvars64.bat"))
                    supported_architectures.push_back({"amd64", CPU::X64, CPU::X64});
                if (fs.exists(vcvarsall_dir / "vcvarsx86_amd64.bat"))
                    supported_architectures.push_back({"x86_amd64", CPU::X86, CPU::X64});
                if (fs.exists(vcvarsall_dir / "vcvarsx86_arm.bat"))
                    supported_architectures.push_back({"x86_arm", CPU::X86, CPU::ARM});
                if (fs.exists(vcvarsall_dir / "vcvarsx86_arm64.bat"))
                    supported_architectures.push_back({"x86_arm64", CPU::X86, CPU::ARM64});
                if (fs.exists(vcvarsall_dir / "vcvarsamd64_x86.bat"))
                    supported_architectures.push_back({"amd64_x86", CPU::X64, CPU::X86});
                if (fs.exists(vcvarsall_dir / "vcvarsamd64_arm.bat"))
                    supported_architectures.push_back({"amd64_arm", CPU::X64, CPU::ARM});
                if (fs.exists(vcvarsall_dir / "vcvarsamd64_arm64.bat"))
                    supported_architectures.push_back({"amd64_arm64", CPU::X64, CPU::ARM64});

                // Locate the "best" MSVC toolchain version
                const fs::path msvc_path = vc_dir / "Tools" / "MSVC";
                std::vector<fs::path> msvc_subdirectories = fs.get_files_non_recursive(msvc_path);
                Util::unstable_keep_if(msvc_subdirectories,
                                       [&fs](const fs::path& path) { return fs.is_directory(path); });

                // Sort them so that latest comes first
                std::sort(
                    msvc_subdirectories.begin(),
                    msvc_subdirectories.end(),
                    [](const fs::path& left, const fs::path& right) { return left.filename() > right.filename(); });

                for (const fs::path& subdir : msvc_subdirectories)
                {
                    const fs::path dumpbin_path = subdir / "bin" / "HostX86" / "x86" / "dumpbin.exe";
                    paths_examined.push_back(dumpbin_path);
                    if (fs.exists(dumpbin_path))
                    {
                        const Toolset v141_toolset{
                            vs_instance.root_path, dumpbin_path, vcvarsall_bat, {}, V_141, supported_architectures};

                        const auto english_language_pack = dumpbin_path.parent_path() / "1033";

                        if (!fs.exists(english_language_pack))
                        {
                            excluded_toolsets.push_back(v141_toolset);
                            break;
                        }

                        found_toolsets.push_back(v141_toolset);

                        if (v140_is_available)
                        {
                            const Toolset v140_toolset{vs_instance.root_path,
                                                       dumpbin_path,
                                                       vcvarsall_bat,
                                                       {"-vcvars_ver=14.0"},
                                                       V_140,
                                                       supported_architectures};
                            found_toolsets.push_back(v140_toolset);
                        }

                        break;
                    }
                }

                continue;
            }

            if (major_version == "14" || major_version == "12")
            {
                const fs::path vcvarsall_bat = vs_instance.root_path / "VC" / "vcvarsall.bat";

                paths_examined.push_back(vcvarsall_bat);
                if (fs.exists(vcvarsall_bat))
                {
                    const fs::path vs_dumpbin_exe = vs_instance.root_path / "VC" / "bin" / "dumpbin.exe";
                    paths_examined.push_back(vs_dumpbin_exe);

                    const fs::path vs_bin_dir = vcvarsall_bat.parent_path() / "bin";
                    std::vector<ToolsetArchOption> supported_architectures;
                    if (fs.exists(vs_bin_dir / "vcvars32.bat"))
                        supported_architectures.push_back({"x86", CPU::X86, CPU::X86});
                    if (fs.exists(vs_bin_dir / "amd64\\vcvars64.bat"))
                        supported_architectures.push_back({"x64", CPU::X64, CPU::X64});
                    if (fs.exists(vs_bin_dir / "x86_amd64\\vcvarsx86_amd64.bat"))
                        supported_architectures.push_back({"x86_amd64", CPU::X86, CPU::X64});
                    if (fs.exists(vs_bin_dir / "x86_arm\\vcvarsx86_arm.bat"))
                        supported_architectures.push_back({"x86_arm", CPU::X86, CPU::ARM});
                    if (fs.exists(vs_bin_dir / "amd64_x86\\vcvarsamd64_x86.bat"))
                        supported_architectures.push_back({"amd64_x86", CPU::X64, CPU::X86});
                    if (fs.exists(vs_bin_dir / "amd64_arm\\vcvarsamd64_arm.bat"))
                        supported_architectures.push_back({"amd64_arm", CPU::X64, CPU::ARM});

                    if (fs.exists(vs_dumpbin_exe))
                    {
                        const Toolset toolset = {vs_instance.root_path,
                                                 vs_dumpbin_exe,
                                                 vcvarsall_bat,
                                                 {},
                                                 major_version == "14" ? V_140 : V_120,
                                                 supported_architectures};

                        const auto english_language_pack = vs_dumpbin_exe.parent_path() / "1033";

                        if (!fs.exists(english_language_pack))
                        {
                            excluded_toolsets.push_back(toolset);
                            break;
                        }

                        found_toolsets.push_back(toolset);
                    }
                }
            }
        }

        if (!excluded_toolsets.empty())
        {
            System::println(
                System::Color::warning,
                "Warning: The following VS instances are excluded because the English language pack is unavailable.");
            for (const Toolset& toolset : excluded_toolsets)
            {
                System::println("    %s", toolset.visual_studio_root_path.u8string());
            }
            System::println(System::Color::warning, "Please install the English language pack.");
        }

        if (found_toolsets.empty())
        {
            System::println(System::Color::error, "Could not locate a complete toolset.");
            System::println("The following paths were examined:");
            for (const fs::path& path : paths_examined)
            {
                System::println("    %s", path.u8string());
            }
            Checks::exit_fail(VCPKG_LINE_INFO);
        }

        return found_toolsets;
    }
}

#endif
