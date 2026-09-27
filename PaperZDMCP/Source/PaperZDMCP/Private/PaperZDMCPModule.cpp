#include "Modules/ModuleManager.h"
#include "PaperZDMCPTools.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FPaperZDMCPModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UToolsetRegistry::RegisterToolsetClass(UPaperZDMCPTools::StaticClass());
    }

    virtual void ShutdownModule() override
    {
        if (UObjectInitialized())
        {
            UToolsetRegistry::UnregisterToolsetClass(UPaperZDMCPTools::StaticClass());
        }
    }
};

IMPLEMENT_MODULE(FPaperZDMCPModule, PaperZDMCP)
