#include "stdafx.h"

#include "GameCommand.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>

namespace
{
    std::vector<const GameCommand*>& Commands()
    {
        static std::vector<const GameCommand*> List;
        return List;
    }
}

GameCommandRegistration::GameCommandRegistration(const GameCommand& Command)
{
    Commands().push_back(&Command);
}

const std::vector<const GameCommand*>& GameCommands()
{
    return Commands();
}

// A game's own commands (e.g. Games/T8/ZoneDiag.cpp), in the help where Main.cpp names them
void AddGameCommands(CliCommandBuilders& B)
{
    B["<game commands>"] = [](CLI::App& App, CliOptions& Options) {
        for (const GameCommand* Command : GameCommands())
        {
            auto* Sub = App.add_subcommand(Command->Name, Command->Help);
            Sub->add_option("zones", Options.DiskExportIn[Command->Name], Command->InputHelp)->required();
            if (Command->AddOptions) Command->AddOptions(*Sub, Options);
        }
    };
}
