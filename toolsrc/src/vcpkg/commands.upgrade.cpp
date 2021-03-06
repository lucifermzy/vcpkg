#include "pch.h"

#include <vcpkg/base/util.h>
#include <vcpkg/commands.h>
#include <vcpkg/dependencies.h>
#include <vcpkg/help.h>
#include <vcpkg/input.h>
#include <vcpkg/install.h>
#include <vcpkg/statusparagraphs.h>
#include <vcpkg/update.h>
#include <vcpkg/vcpkglib.h>

namespace vcpkg::Commands::Upgrade
{
    using Install::KeepGoing;
    using Install::to_keep_going;

    static const std::string OPTION_NO_DRY_RUN = "--no-dry-run";
    static const std::string OPTION_KEEP_GOING = "--keep-going";

    static const std::array<CommandSwitch, 2> INSTALL_SWITCHES = {{
        {OPTION_NO_DRY_RUN, "Actually upgrade"},
        {OPTION_KEEP_GOING, "Continue installing packages on failure"},
    }};

    const CommandStructure COMMAND_STRUCTURE = {
        Help::create_example_string("upgrade --no-dry-run"),
        0,
        SIZE_MAX,
        {INSTALL_SWITCHES, {}},
        nullptr,
    };

    void perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths, const Triplet& default_triplet)
    {
        const ParsedArguments options = args.parse_arguments(COMMAND_STRUCTURE);

        const bool no_dry_run = Util::Sets::contains(options.switches, OPTION_NO_DRY_RUN);
        const KeepGoing keep_going = to_keep_going(Util::Sets::contains(options.switches, OPTION_KEEP_GOING));

        StatusParagraphs status_db = database_load_check(paths);

        Dependencies::PathsPortFileProvider provider(paths);
        Dependencies::PackageGraph graph(provider, status_db);

        // input sanitization
        const std::vector<PackageSpec> specs = Util::fmap(args.command_arguments, [&](auto&& arg) {
            return Input::check_and_get_package_spec(arg, default_triplet, COMMAND_STRUCTURE.example_text);
        });

        for (auto&& spec : specs)
        {
            Input::check_triplet(spec.triplet(), paths);
        }

        if (specs.empty())
        {
            // If no packages specified, upgrade all outdated packages.
            auto outdated_packages = Update::find_outdated_packages(provider, status_db);

            if (outdated_packages.empty())
            {
                System::println("All installed packages are up-to-date with the local portfiles.");
                Checks::exit_success(VCPKG_LINE_INFO);
            }

            for (auto&& outdated_package : outdated_packages)
                graph.upgrade(outdated_package.spec);
        }
        else
        {
            std::vector<PackageSpec> not_installed;
            std::vector<PackageSpec> no_portfile;
            std::vector<PackageSpec> to_upgrade;
            std::vector<PackageSpec> up_to_date;

            for (auto&& spec : specs)
            {
                auto it = status_db.find_installed(spec);
                if (it == status_db.end())
                {
                    not_installed.push_back(spec);
                }

                auto maybe_scf = provider.get_control_file(spec.name());
                if (auto p_scf = maybe_scf.get())
                {
                    if (it != status_db.end())
                    {
                        if (p_scf->core_paragraph->version != (*it)->package.version)
                        {
                            to_upgrade.push_back(spec);
                        }
                        else
                        {
                            up_to_date.push_back(spec);
                        }
                    }
                }
                else
                {
                    no_portfile.push_back(spec);
                }
            }

            Util::sort(not_installed);
            Util::sort(no_portfile);
            Util::sort(up_to_date);
            Util::sort(to_upgrade);

            if (!up_to_date.empty())
            {
                System::println(System::Color::success, "The following packages are up-to-date:");
                System::println(Strings::join(
                    "", up_to_date, [](const PackageSpec& spec) { return "    " + spec.to_string() + "\n"; }));
            }

            if (!not_installed.empty())
            {
                System::println(System::Color::error, "The following packages are not installed:");
                System::println(Strings::join(
                    "", not_installed, [](const PackageSpec& spec) { return "    " + spec.to_string() + "\n"; }));
            }

            if (!no_portfile.empty())
            {
                System::println(System::Color::error, "The following packages do not have a valid portfile:");
                System::println(Strings::join(
                    "", no_portfile, [](const PackageSpec& spec) { return "    " + spec.to_string() + "\n"; }));
            }

            Checks::check_exit(VCPKG_LINE_INFO, not_installed.empty() && no_portfile.empty());

            if (to_upgrade.empty()) Checks::exit_success(VCPKG_LINE_INFO);

            for (auto&& spec : to_upgrade)
                graph.upgrade(spec);
        }

        auto plan = graph.serialize();

        Checks::check_exit(VCPKG_LINE_INFO, !plan.empty());

        const Build::BuildPackageOptions install_plan_options = {
            Build::UseHeadVersion::NO,
            Build::AllowDownloads::YES,
            Build::CleanBuildtrees::NO,
        };

        // Set build settings for all install actions
        for (auto&& action : plan)
        {
            if (auto p_install = action.install_action.get())
            {
                p_install->build_options = install_plan_options;
            }
        }

        Dependencies::print_plan(plan, true);

        if (!no_dry_run)
        {
            System::println(System::Color::warning,
                            "If you are sure you want to rebuild the above packages, run this command with the "
                            "--no-dry-run option.");
            Checks::exit_fail(VCPKG_LINE_INFO);
        }

        const Install::InstallSummary summary = Install::perform(plan, keep_going, paths, status_db);

        System::println("\nTotal elapsed time: %s\n", summary.total_elapsed_time);

        if (keep_going == KeepGoing::YES)
        {
            summary.print();
        }

        Checks::exit_success(VCPKG_LINE_INFO);
    }
}
