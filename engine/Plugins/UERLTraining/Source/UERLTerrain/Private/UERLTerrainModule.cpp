#include "Modules/ModuleManager.h"

#include "UERLSubTerrainGenerator.h"

class FUERLPlaneSubTerrainGenerator;
class FUERLHeightfieldSubTerrainGenerator;
class FUERLBoxesSubTerrainGenerator;

TSharedRef<IUERLSubTerrainGenerator> MakePlaneSubTerrainGenerator();
TSharedRef<IUERLSubTerrainGenerator> MakeHeightfieldSubTerrainGenerator();
TSharedRef<IUERLSubTerrainGenerator> MakeBoxesSubTerrainGenerator();

class FUERLTerrainModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterSubTerrainGenerator(MakePlaneSubTerrainGenerator());
		RegisterSubTerrainGenerator(MakeHeightfieldSubTerrainGenerator());
		RegisterSubTerrainGenerator(MakeBoxesSubTerrainGenerator());
	}
};

IMPLEMENT_MODULE(FUERLTerrainModule, UERLTerrain);
