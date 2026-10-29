#include "ps2recomp/ps2_recompiler.h"
#include <iostream>
#include <string>
#include <memory>
#include "ps2recomp/producer_dependencies.h"
#include <iomanip>

using namespace ps2recomp;

void printUsage()
{
    std::cout << "PS2Recomp - A static recompiler for PlayStation 2 ELF files\n";
    std::cout << "Usage: ps2recomp <config.toml>\n";
    std::cout << "  config.toml: Configuration file for the recompiler\n";
}

int main(int argc, char *argv[])
{
    if(argc==2 && std::string(argv[1])=="--producer-dependencies")
    {
        try
        {
            std::cout<<"PS2RECOMP_PRODUCER_DEPS 1\n";
            for(const auto &path:producerDependencies())
            {
                const auto u8=path.generic_u8string();
                std::cout<<std::quoted(std::string(u8.begin(),u8.end()))<<'\n';
            }
            return 0;
        }
        catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
    }
    if (argc < 2)
    {
        printUsage();
        return 1;
    }

    std::string configPath = argv[1];
    bool timed = true;
    std::string run = timing::newRunId(), profile, identity = "ps2_recomp";
    for (int i = 2; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "--no-timing") timed = false;
        else if ((option == "--timing-run" || option == "--timing-profile" || option == "--timing-identity") && i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (option == "--timing-run") run = value;
            else if (option == "--timing-profile") profile = value;
            else identity = value;
        }
        else { std::cerr << "Unknown/incomplete option: " << option << '\n'; return 1; }
    }
    std::unique_ptr<timing::Session> session;
    if (timed) session = std::make_unique<timing::Session>(std::cout, run, identity, profile,
                                                         timing::Session::Clock{}, "[setup-timing] ");
    timing::Stage total(session.get(), "translator");

    try
    {
        PS2Recompiler recompiler(configPath);
        recompiler.setTiming(session.get(), total.id());

        if (!recompiler.initialize())
        {
            std::cerr << "Failed to initialize recompiler\n";
            recompiler.printReport();
            total.finish("failed");
            return 1;
        }

        if (!recompiler.recompile())
        {
            std::cerr << "Recompilation failed\n";
            recompiler.printReport();
            total.finish("failed");
            return 1;
        }

        recompiler.generateOutput();
        recompiler.printReport();

        std::cout << "Recompilation completed successfully\n";
        total.finish();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        total.finish("failed");
        return 1;
    }
}
