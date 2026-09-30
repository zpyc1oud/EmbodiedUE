#include "UERLTerrainAtlas.h"
#include "UERLSubTerrainGenerator.h"
#include "UERLTerrainInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UERLTerrainInternal
{
	struct FPerlinParameters
	{
		double PerlinScale = 0.0;
		double PerlinPersistence = 0.0;
		double PerlinLacunarity = 0.0;
		int32 PerlinOctaves = 0;
		bool bUsePerlin = false;
	};

	struct FHeightfieldParameters : FPerlinParameters
	{
		FVector2D NoiseRange = FVector2D::ZeroVector;
		double NoiseStep = 0.0;
		double HorizontalScale = 0.0;
		double VerticalScale = 0.0;
		double SampleScale = 0.0;
	};

	struct FBoxesParameters : FPerlinParameters
	{
		double GridWidth = 0.0;
		FVector2D HeightRange = FVector2D::ZeroVector;
		double Difficulty = 0.0;
		bool bHoles = false;
		bool bUseRandomGrid = false;
		bool bExplicit = false;
		TArray<FUERLTerrainBoxSpec> ExplicitBoxes;
	};

	constexpr double CentimetresPerMetre = 100.0;
	constexpr double FlatBoxHeightMetres = 0.01;

	bool HasExactKeys(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Names)
	{
		if (!Object || Object->Values.Num() != static_cast<int32>(Names.size()))
		{
			return false;
		}
		for (const TCHAR* Name : Names)
		{
			if (!Object->HasField(Name))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, double& OutValue)
	{
		return Object && Object->TryGetNumberField(Name, OutValue) && FMath::IsFinite(OutValue);
	}

	bool ReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, bool& OutValue)
	{
		return Object && Object->TryGetBoolField(Name, OutValue);
	}

	bool ReadRange(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FVector2D& OutRange)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object || !Object->TryGetArrayField(Name, Values) || !Values || Values->Num() != 2)
		{
			return false;
		}
		double MinValue = 0.0;
		double MaxValue = 0.0;
		const TSharedPtr<FJsonValue>& MinJson = (*Values)[0];
		const TSharedPtr<FJsonValue>& MaxJson = (*Values)[1];
		if (!MinJson.IsValid() || !MaxJson.IsValid()
			|| MinJson->Type != EJson::Number || MaxJson->Type != EJson::Number)
		{
			return false;
		}
		MinValue = MinJson->AsNumber();
		MaxValue = MaxJson->AsNumber();
		if (!FMath::IsFinite(MinValue) || !FMath::IsFinite(MaxValue) || MinValue > MaxValue)
		{
			return false;
		}
		OutRange = FVector2D(MinValue, MaxValue);
		return true;
	}

	bool ReadVector(const TSharedPtr<FJsonValue>& Value, int32 Size, TArray<double>& OutValues)
	{
		if (!Value.IsValid() || Value->Type != EJson::Array)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
		if (Values.Num() != Size)
		{
			return false;
		}
		OutValues.Reset(Size);
		for (const TSharedPtr<FJsonValue>& Item : Values)
		{
			if (!Item.IsValid() || Item->Type != EJson::Number || !FMath::IsFinite(Item->AsNumber()))
			{
				return false;
			}
			OutValues.Add(Item->AsNumber());
		}
		return true;
	}

	bool HasPerlinFields(const TSharedPtr<FJsonObject>& Object)
	{
		return Object && (
			Object->HasField(TEXT("perlin_scale"))
			|| Object->HasField(TEXT("perlin_octaves"))
			|| Object->HasField(TEXT("perlin_persistence"))
			|| Object->HasField(TEXT("perlin_lacunarity")));
	}

	bool ParsePerlinParameters(
		const TSharedPtr<FJsonObject>& Object,
		FPerlinParameters& Out,
		FString& OutError)
	{
		double Octaves = 0.0;
		if (!ReadNumber(Object, TEXT("perlin_scale"), Out.PerlinScale)
			|| !ReadNumber(Object, TEXT("perlin_octaves"), Octaves)
			|| !ReadNumber(Object, TEXT("perlin_persistence"), Out.PerlinPersistence)
			|| !ReadNumber(Object, TEXT("perlin_lacunarity"), Out.PerlinLacunarity)
			|| Out.PerlinScale <= 0.0
			|| Octaves < 1.0 || Octaves > 8.0 || Octaves != FMath::RoundToDouble(Octaves)
			|| Out.PerlinPersistence <= 0.0 || Out.PerlinPersistence > 1.0
			|| Out.PerlinLacunarity <= 1.0)
		{
			OutError = TEXT("terrain Perlin parameters are invalid");
			return false;
		}
		Out.PerlinOctaves = FMath::RoundToInt(Octaves);
		Out.bUsePerlin = true;
		return true;
	}

	bool ParseHeightfieldParameters(
		const TSharedPtr<FJsonObject>& Object,
		FHeightfieldParameters& Out,
		FString& OutError)
	{
		const bool bHasPerlin = HasPerlinFields(Object);
		if ((!bHasPerlin && !HasExactKeys(Object, {
			TEXT("noise_range"), TEXT("noise_step"), TEXT("horizontal_scale"),
			TEXT("vertical_scale"), TEXT("downsampled_scale") }))
			|| (bHasPerlin && !HasExactKeys(Object, {
			TEXT("noise_range"), TEXT("noise_step"), TEXT("horizontal_scale"),
			TEXT("vertical_scale"), TEXT("downsampled_scale"),
			TEXT("perlin_scale"), TEXT("perlin_octaves"), TEXT("perlin_persistence"),
			TEXT("perlin_lacunarity") })))
		{
			OutError = TEXT("heightfield params do not match the terrain contract");
			return false;
		}
		if (!ReadRange(Object, TEXT("noise_range"), Out.NoiseRange)
			|| !ReadNumber(Object, TEXT("noise_step"), Out.NoiseStep)
			|| !ReadNumber(Object, TEXT("horizontal_scale"), Out.HorizontalScale)
			|| !ReadNumber(Object, TEXT("vertical_scale"), Out.VerticalScale)
			|| Out.NoiseStep <= 0.0 || Out.HorizontalScale <= 0.0 || Out.VerticalScale <= 0.0)
		{
			OutError = TEXT("heightfield params contain invalid noise or scale values");
			return false;
		}

		const TSharedPtr<FJsonValue>* DownsampledValue = Object->Values.Find(TEXT("downsampled_scale"));
		if (!DownsampledValue || !DownsampledValue->IsValid())
		{
			OutError = TEXT("heightfield params are missing downsampled_scale");
			return false;
		}
		if ((*DownsampledValue)->Type == EJson::Null)
		{
			Out.SampleScale = Out.HorizontalScale;
		}
		else if ((*DownsampledValue)->Type == EJson::Number
			&& FMath::IsFinite((*DownsampledValue)->AsNumber()))
		{
			Out.SampleScale = (*DownsampledValue)->AsNumber();
		}
		else
		{
			OutError = TEXT("heightfield downsampled_scale must be a finite number or null");
			return false;
		}
		if (Out.SampleScale < Out.HorizontalScale)
		{
			OutError = TEXT("heightfield downsampled_scale must not be smaller than horizontal_scale");
			return false;
		}
		if (!bHasPerlin)
		{
			return true;
		}

		return ParsePerlinParameters(Object, Out, OutError);
	}

	bool ParseBoxesParameters(
		const TSharedPtr<FJsonObject>& Object,
		FBoxesParameters& Out,
		FString& OutError)
	{
		if (!Object)
		{
			OutError = TEXT("boxes params are missing");
			return false;
		}
		if (Object->HasField(TEXT("explicit")))
		{
			if (!HasExactKeys(Object, { TEXT("explicit") }))
			{
				OutError = TEXT("boxes explicit params contain unexpected keys");
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Explicit = nullptr;
			if (!Object->TryGetArrayField(TEXT("explicit"), Explicit) || !Explicit)
			{
				OutError = TEXT("boxes explicit must be an array");
				return false;
			}
			Out.bExplicit = true;
			Out.ExplicitBoxes.Reserve(Explicit->Num());
			for (const TSharedPtr<FJsonValue>& Value : *Explicit)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					OutError = TEXT("boxes explicit entries must be objects");
					return false;
				}
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (!HasExactKeys(Entry, { TEXT("pose"), TEXT("extent") }))
				{
					OutError = TEXT("boxes explicit entry does not match the terrain contract");
					return false;
				}
				const TSharedPtr<FJsonValue>* PoseValue = Entry->Values.Find(TEXT("pose"));
				const TSharedPtr<FJsonValue>* ExtentValue = Entry->Values.Find(TEXT("extent"));
				TArray<double> Pose;
				TArray<double> Extent;
				if (!PoseValue || !ExtentValue || !ReadVector(*PoseValue, 4, Pose) || !ReadVector(*ExtentValue, 3, Extent)
					|| Extent[0] <= 0.0 || Extent[1] <= 0.0 || Extent[2] <= 0.0)
				{
					OutError = TEXT("boxes explicit pose or extent is invalid");
					return false;
				}
				FUERLTerrainBoxSpec& Box = Out.ExplicitBoxes.AddDefaulted_GetRef();
				Box.CenterMeters = FVector(Pose[0], Pose[1], Pose[2]);
				Box.ExtentMeters = FVector(Extent[0], Extent[1], Extent[2]);
				Box.YawRadians = Pose[3];
			}
			return true;
		}
		if (Object->HasField(TEXT("generator")))
		{
			if (!HasExactKeys(Object, {
				TEXT("grid_width"), TEXT("grid_height_range"), TEXT("holes"),
				TEXT("generator"), TEXT("difficulty") }))
			{
				OutError = TEXT("boxes random-grid params do not match the terrain contract");
				return false;
			}
			FString Generator;
			if (!Object->TryGetStringField(TEXT("generator"), Generator)
				|| Generator != TEXT("random_grid")
				|| !ReadNumber(Object, TEXT("difficulty"), Out.Difficulty)
				|| Out.Difficulty < 0.0 || Out.Difficulty > 1.0)
			{
				OutError = TEXT("boxes random-grid generator or difficulty is invalid");
				return false;
			}
			if (!ReadNumber(Object, TEXT("grid_width"), Out.GridWidth)
				|| !ReadRange(Object, TEXT("grid_height_range"), Out.HeightRange)
				|| !ReadBool(Object, TEXT("holes"), Out.bHoles)
				|| Out.GridWidth <= 0.0)
			{
				OutError = TEXT("boxes params contain invalid grid or height values");
				return false;
			}
			Out.bUseRandomGrid = true;
			return true;
		}

		const bool bHasPerlin = HasPerlinFields(Object);
		if ((!bHasPerlin && !HasExactKeys(Object, {
			TEXT("grid_width"), TEXT("grid_height_range"), TEXT("holes") }))
			|| (bHasPerlin && !HasExactKeys(Object, {
			TEXT("grid_width"), TEXT("grid_height_range"), TEXT("holes"),
			TEXT("perlin_scale"), TEXT("perlin_octaves"), TEXT("perlin_persistence"),
			TEXT("perlin_lacunarity") })))
		{
			OutError = TEXT("boxes params do not match the terrain contract");
			return false;
		}
		if (!ReadNumber(Object, TEXT("grid_width"), Out.GridWidth)
			|| !ReadRange(Object, TEXT("grid_height_range"), Out.HeightRange)
			|| !ReadBool(Object, TEXT("holes"), Out.bHoles)
			|| Out.GridWidth <= 0.0)
		{
			OutError = TEXT("boxes params contain invalid grid or height values");
			return false;
		}
		return !bHasPerlin || ParsePerlinParameters(Object, Out, OutError);
	}

	uint64 Mix(uint64 Value)
	{
		Value ^= Value >> 30;
		Value *= 0xbf58476d1ce4e5b9ULL;
		Value ^= Value >> 27;
		Value *= 0x94d049bb133111ebULL;
		return Value ^ (Value >> 31);
	}

	uint64 HashGrid(uint64 Seed, int32 SlotId, int32 X, int32 Y)
	{
		uint64 Value = Mix(Seed + 0x9e3779b97f4a7c15ULL);
		Value = Mix(Value ^ static_cast<uint64>(SlotId + 1));
		Value = Mix(Value ^ static_cast<uint64>(X + 1));
		return Mix(Value ^ static_cast<uint64>(Y + 1));
	}

	double UnitSample(uint64 Seed, int32 SlotId, int32 X, int32 Y)
	{
		constexpr uint64 MantissaMask = (uint64(1) << 53) - 1;
		return static_cast<double>(HashGrid(Seed, SlotId, X, Y) & MantissaMask)
			/ static_cast<double>(uint64(1) << 53);
	}

	// Local CC0 port of the 2D evaluator from KdotJPG/OpenSimplex2S; the
	// surrounding fBm and terrain mapping remain owned by this module.
	constexpr uint64 OpenSimplex2SPrimeX = 0x5205402B9270C86FULL;
	constexpr uint64 OpenSimplex2SPrimeY = 0x598CD327003817B5ULL;
	constexpr uint64 OpenSimplex2SHashMultiplier = 0x53A3F72DEEC546F5ULL;
	constexpr double OpenSimplex2SSkew2D = 0.366025403784439;
	constexpr double OpenSimplex2SUnskew2D = -0.21132486540518713;
	constexpr double OpenSimplex2SRadiusSquared2D = 2.0 / 3.0;
	constexpr double OpenSimplex2SGradientNormalizer2D = 0.05481866495625118;

	// The official OpenSimplex2S 2D gradient set has 24 evenly distributed
	// directions.  Keeping the table fixed avoids per-sample trigonometry and
	// gives the rasterizer a deterministic, isotropic source field.
	constexpr double OpenSimplex2SGradients2D[24][2] = {
		{ 0.38268343236509,  0.923879532511287 },
		{ 0.923879532511287, 0.38268343236509 },
		{ 0.923879532511287, -0.38268343236509 },
		{ 0.38268343236509, -0.923879532511287 },
		{ -0.38268343236509, -0.923879532511287 },
		{ -0.923879532511287, -0.38268343236509 },
		{ -0.923879532511287, 0.38268343236509 },
		{ -0.38268343236509, 0.923879532511287 },
		{ 0.130526192220052, 0.99144486137381 },
		{ 0.608761429008721, 0.793353340291235 },
		{ 0.793353340291235, 0.608761429008721 },
		{ 0.99144486137381, 0.130526192220051 },
		{ 0.99144486137381, -0.130526192220051 },
		{ 0.793353340291235, -0.60876142900872 },
		{ 0.608761429008721, -0.793353340291235 },
		{ 0.130526192220052, -0.99144486137381 },
		{ -0.130526192220052, -0.99144486137381 },
		{ -0.608761429008721, -0.793353340291235 },
		{ -0.793353340291235, -0.608761429008721 },
		{ -0.99144486137381, -0.130526192220052 },
		{ -0.99144486137381, 0.130526192220051 },
		{ -0.793353340291235, 0.608761429008721 },
		{ -0.608761429008721, 0.793353340291235 },
		{ -0.130526192220052, 0.99144486137381 },
	};

	uint64 OpenSimplex2SHash(uint64 Seed, uint64 XPrime, uint64 YPrime)
	{
		uint64 Hash = Seed ^ XPrime ^ YPrime;
		Hash *= OpenSimplex2SHashMultiplier;
		// Java's reference implementation uses a signed right shift.  Spell out
		// the sign extension so the C++ port remains defined for uint64.
		const uint64 Shifted = (Hash >> 58)
			| ((Hash & 0x8000000000000000ULL) != 0 ? 0xFFFFFFFFFFFFFFC0ULL : 0ULL);
		Hash ^= Shifted;
		return Hash;
	}

	double OpenSimplex2SGradientDot(uint64 Seed, uint64 XPrime, uint64 YPrime, double OffsetX, double OffsetY)
	{
		const uint64 Hash = OpenSimplex2SHash(Seed, XPrime, YPrime);
		const uint32 GradientSlot = static_cast<uint32>(Hash) & 0xFEU;
		const int32 GradientIndex = static_cast<int32>(GradientSlot >> 1) % 24;
		const double* Gradient = OpenSimplex2SGradients2D[GradientIndex];
		return (Gradient[0] * OffsetX + Gradient[1] * OffsetY)
			/ OpenSimplex2SGradientNormalizer2D;
	}

	double OpenSimplex2S2D(uint64 Seed, double X, double Y)
	{
		const double Skew = OpenSimplex2SSkew2D * (X + Y);
		const double Xs = X + Skew;
		const double Ys = Y + Skew;
		const int32 Xsb = FMath::FloorToInt(Xs);
		const int32 Ysb = FMath::FloorToInt(Ys);
		const float Xi = static_cast<float>(Xs - Xsb);
		const float Yi = static_cast<float>(Ys - Ysb);
		const uint64 XsbPrime = static_cast<uint64>(Xsb) * OpenSimplex2SPrimeX;
		const uint64 YsbPrime = static_cast<uint64>(Ysb) * OpenSimplex2SPrimeY;

		const float T = (Xi + Yi) * static_cast<float>(OpenSimplex2SUnskew2D);
		const float Dx0 = Xi + T;
		const float Dy0 = Yi + T;
		const float A0 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
			- Dx0 * Dx0 - Dy0 * Dy0;
		double Value = (A0 * A0) * (A0 * A0)
			* OpenSimplex2SGradientDot(Seed, XsbPrime, YsbPrime, Dx0, Dy0);

		const float A1 = static_cast<float>(
			2.0 * (1.0 + 2.0 * OpenSimplex2SUnskew2D)
			* (1.0 / OpenSimplex2SUnskew2D + 2.0)) * T
			+ (static_cast<float>(
				-2.0 * (1.0 + 2.0 * OpenSimplex2SUnskew2D)
				* (1.0 + 2.0 * OpenSimplex2SUnskew2D)) + A0);
		const float Dx1 = Dx0 - static_cast<float>(1.0 + 2.0 * OpenSimplex2SUnskew2D);
		const float Dy1 = Dy0 - static_cast<float>(1.0 + 2.0 * OpenSimplex2SUnskew2D);
		Value += (A1 * A1) * (A1 * A1)
			* OpenSimplex2SGradientDot(
				Seed, XsbPrime + OpenSimplex2SPrimeX, YsbPrime + OpenSimplex2SPrimeY, Dx1, Dy1);

		const float XMinusY = Xi - Yi;
		if (T < OpenSimplex2SUnskew2D)
		{
			if (Xi + XMinusY > 1.0f)
			{
				const float Dx2 = Dx0 - static_cast<float>(3.0 * OpenSimplex2SUnskew2D + 2.0);
				const float Dy2 = Dy0 - static_cast<float>(3.0 * OpenSimplex2SUnskew2D + 1.0);
				const float A2 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx2 * Dx2 - Dy2 * Dy2;
				if (A2 > 0.0f)
				{
					Value += (A2 * A2) * (A2 * A2)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime + (OpenSimplex2SPrimeX << 1), YsbPrime + OpenSimplex2SPrimeY, Dx2, Dy2);
				}
			}
			else
			{
				const float Dx2 = Dx0 - static_cast<float>(OpenSimplex2SUnskew2D);
				const float Dy2 = Dy0 - static_cast<float>(OpenSimplex2SUnskew2D + 1.0);
				const float A2 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx2 * Dx2 - Dy2 * Dy2;
				if (A2 > 0.0f)
				{
					Value += (A2 * A2) * (A2 * A2)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime, YsbPrime + OpenSimplex2SPrimeY, Dx2, Dy2);
				}
			}
			if (Yi - XMinusY > 1.0f)
			{
				const float Dx3 = Dx0 - static_cast<float>(3.0 * OpenSimplex2SUnskew2D + 1.0);
				const float Dy3 = Dy0 - static_cast<float>(3.0 * OpenSimplex2SUnskew2D + 2.0);
				const float A3 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx3 * Dx3 - Dy3 * Dy3;
				if (A3 > 0.0f)
				{
					Value += (A3 * A3) * (A3 * A3)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime + OpenSimplex2SPrimeX, YsbPrime + (OpenSimplex2SPrimeY << 1), Dx3, Dy3);
				}
			}
			else
			{
				const float Dx3 = Dx0 - static_cast<float>(OpenSimplex2SUnskew2D + 1.0);
				const float Dy3 = Dy0 - static_cast<float>(OpenSimplex2SUnskew2D);
				const float A3 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx3 * Dx3 - Dy3 * Dy3;
				if (A3 > 0.0f)
				{
					Value += (A3 * A3) * (A3 * A3)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime + OpenSimplex2SPrimeX, YsbPrime, Dx3, Dy3);
				}
			}
		}
		else
		{
			if (Xi + XMinusY < 0.0f)
			{
				const float Dx2 = Dx0 + static_cast<float>(1.0 + OpenSimplex2SUnskew2D);
				const float Dy2 = Dy0 + static_cast<float>(OpenSimplex2SUnskew2D);
				const float A2 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx2 * Dx2 - Dy2 * Dy2;
				if (A2 > 0.0f)
				{
					Value += (A2 * A2) * (A2 * A2)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime - OpenSimplex2SPrimeX, YsbPrime, Dx2, Dy2);
				}
			}
			else
			{
				const float Dx2 = Dx0 - static_cast<float>(OpenSimplex2SUnskew2D + 1.0);
				const float Dy2 = Dy0 - static_cast<float>(OpenSimplex2SUnskew2D);
				const float A2 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx2 * Dx2 - Dy2 * Dy2;
				if (A2 > 0.0f)
				{
					Value += (A2 * A2) * (A2 * A2)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime + OpenSimplex2SPrimeX, YsbPrime, Dx2, Dy2);
				}
			}
			if (Yi < XMinusY)
			{
				const float Dx3 = Dx0 + static_cast<float>(OpenSimplex2SUnskew2D);
				const float Dy3 = Dy0 + static_cast<float>(OpenSimplex2SUnskew2D + 1.0);
				const float A3 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx3 * Dx3 - Dy3 * Dy3;
				if (A3 > 0.0f)
				{
					Value += (A3 * A3) * (A3 * A3)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime, YsbPrime - OpenSimplex2SPrimeY, Dx3, Dy3);
				}
			}
			else
			{
				const float Dx3 = Dx0 - static_cast<float>(OpenSimplex2SUnskew2D);
				const float Dy3 = Dy0 - static_cast<float>(OpenSimplex2SUnskew2D + 1.0);
				const float A3 = static_cast<float>(OpenSimplex2SRadiusSquared2D)
					- Dx3 * Dx3 - Dy3 * Dy3;
				if (A3 > 0.0f)
				{
					Value += (A3 * A3) * (A3 * A3)
						* OpenSimplex2SGradientDot(
							Seed, XsbPrime, YsbPrime + OpenSimplex2SPrimeY, Dx3, Dy3);
				}
			}
		}
		return Value;
	}

	double OpenSimplex2SFractal(
		uint64 Seed,
		int32 SlotId,
		double X,
		double Y,
		int32 Octaves,
		double Persistence,
		double Lacunarity)
	{
		const uint64 SlotSeed = Mix(Seed ^ (static_cast<uint64>(SlotId + 1) * 0x9e3779b97f4a7c15ULL));
		double Value = 0.0;
		double Amplitude = 1.0;
		double Frequency = 1.0;
		double Normalizer = 0.0;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			Value += Amplitude * OpenSimplex2S2D(
				SlotSeed + static_cast<uint64>(Octave) * 0x9e3779b97f4a7c15ULL,
				X * Frequency,
				Y * Frequency);
			Normalizer += Amplitude;
			Amplitude *= Persistence;
			Frequency *= Lacunarity;
		}
		checkf(Normalizer > 0.0, TEXT("OpenSimplex2S fractal normalizer must be positive"));
		return Value / Normalizer;
	}

	double OpenSimplex2SUnit(
		const FPerlinParameters& Parameters,
		uint64 Seed,
		int32 SlotId,
		double SampleX,
		double SampleY,
		double PhaseX,
		double PhaseY)
	{
		const double Noise = FMath::Clamp(
			OpenSimplex2SFractal(
				Seed,
				SlotId,
				SampleX / Parameters.PerlinScale + PhaseX,
				SampleY / Parameters.PerlinScale + PhaseY,
				Parameters.PerlinOctaves,
				Parameters.PerlinPersistence,
				Parameters.PerlinLacunarity),
			-1.0,
			1.0);
		return 0.5 * (Noise + 1.0);
	}

	double PerlinFade(double T)
	{
		return T * T * T * (T * (T * 6.0 - 15.0) + 10.0);
	}

	double PerlinGradientDot(uint64 Seed, int32 SlotId, int32 X, int32 Y, double OffsetX, double OffsetY)
	{
		constexpr double TwoPi = 6.28318530717958647692;
		const double Angle = UnitSample(Seed, SlotId, X, Y) * TwoPi;
		return FMath::Cos(Angle) * OffsetX + FMath::Sin(Angle) * OffsetY;
	}

	double PerlinSingle(uint64 Seed, int32 SlotId, double X, double Y)
	{
		const int32 X0 = FMath::FloorToInt(X);
		const int32 Y0 = FMath::FloorToInt(Y);
		const int32 X1 = X0 + 1;
		const int32 Y1 = Y0 + 1;
		const double Tx = X - X0;
		const double Ty = Y - Y0;
		const double U = PerlinFade(Tx);
		const double V = PerlinFade(Ty);
		const double N00 = PerlinGradientDot(Seed, SlotId, X0, Y0, Tx, Ty);
		const double N10 = PerlinGradientDot(Seed, SlotId, X1, Y0, Tx - 1.0, Ty);
		const double N01 = PerlinGradientDot(Seed, SlotId, X0, Y1, Tx, Ty - 1.0);
		const double N11 = PerlinGradientDot(Seed, SlotId, X1, Y1, Tx - 1.0, Ty - 1.0);
		return FMath::Lerp(FMath::Lerp(N00, N10, U), FMath::Lerp(N01, N11, U), V);
	}

	double PerlinFractal(
		uint64 Seed,
		int32 SlotId,
		double X,
		double Y,
		int32 Octaves,
		double Persistence,
		double Lacunarity)
	{
		double Value = 0.0;
		double Amplitude = 1.0;
		double Frequency = 1.0;
		double Normalizer = 0.0;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			Value += Amplitude * PerlinSingle(Seed + static_cast<uint64>(Octave) * 0x9e3779b97f4a7c15ULL,
				SlotId, X * Frequency, Y * Frequency);
			Normalizer += Amplitude;
			Amplitude *= Persistence;
			Frequency *= Lacunarity;
		}
		checkf(Normalizer > 0.0, TEXT("Perlin fractal normalizer must be positive"));
		return Value / Normalizer;
	}

	double PerlinUnit(
		const FPerlinParameters& Parameters,
		uint64 Seed,
		int32 SlotId,
		double SampleX,
		double SampleY,
		double PhaseX,
		double PhaseY)
	{
		// Unit-length 2D gradients normally produce a field close to +/- 1/3.
		// Expand that nominal range before mapping to the configured interval so
		// level 3 can use the height range specified by the terrain profile.
		constexpr double PerlinOutputGain = 3.0;
		const double Noise = FMath::Clamp(
			PerlinFractal(
				Seed,
				SlotId,
				SampleX / Parameters.PerlinScale + PhaseX,
				SampleY / Parameters.PerlinScale + PhaseY,
				Parameters.PerlinOctaves,
				Parameters.PerlinPersistence,
				Parameters.PerlinLacunarity),
			-1.0 / PerlinOutputGain,
			1.0 / PerlinOutputGain) * PerlinOutputGain;
		return 0.5 * (Noise + 1.0);
	}

	double Quantize(double Value, double Step)
	{
		return FMath::RoundToDouble(Value / Step) * Step;
	}

	// Hermite smoothstep. Used as the interpolation weight between coarse
	// heightfield samples so the resulting surface has continuous slope.
	double SmoothStepWeight(double T)
	{
		return T * T * (3.0 - 2.0 * T);
	}

	FVector PatchOriginMeters(const FUERLTerrainConfig& Config, int32 Level, const FVector& SlotOriginCm)
	{
		const double TierStrideX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
		return SlotOriginCm / CentimetresPerMetre
			+ FVector(Level * TierStrideX, 0.0, 0.0);
	}

	void AddPlane(
		const FUERLTerrainConfig& Config,
		int32 SlotId,
		const FVector& PatchOrigin,
		FUERLTerrainTierPlan& OutPlan)
	{
		const double SizeX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
		const double SizeY = Config.CellSize[1] + 2.0 * Config.BorderWidth;
		FUERLTerrainBoxSpec& Plane = OutPlan.Boxes.AddDefaulted_GetRef();
		Plane.SlotId = SlotId;
		Plane.CenterMeters = PatchOrigin + FVector(0.0, 0.0, -FlatBoxHeightMetres * 0.5);
		Plane.ExtentMeters = FVector(SizeX * 0.5, SizeY * 0.5, FlatBoxHeightMetres * 0.5);
	}

	void AppendIndexedCollisionQuad(
		FUERLTerrainMeshSpec& OutMesh,
		int32 A,
		int32 B,
		int32 C,
		int32 D)
	{
		// UProceduralMeshComponent uses the reversed winding expected by UE's
		// left-handed collision convention.
		OutMesh.Triangles.Append({ A, C, B, A, D, C });
	}

	void AppendSolidBoxCollision(
		const FUERLTerrainBoxSpec& Spec,
		FUERLTerrainMeshSpec& OutMesh)
	{
		const FVector Center = Spec.CenterMeters;
		const FVector Extent = Spec.ExtentMeters;
		const double CosYaw = FMath::Cos(Spec.YawRadians);
		const double SinYaw = FMath::Sin(Spec.YawRadians);
		const auto Transform = [Center, CosYaw, SinYaw](const FVector& Local)
		{
			return Center + FVector(
				CosYaw * Local.X - SinYaw * Local.Y,
				SinYaw * Local.X + CosYaw * Local.Y,
				Local.Z);
		};

		const double X = Extent.X;
		const double Y = Extent.Y;
		const double Z = Extent.Z;
		const int32 BaseVertex = OutMesh.VerticesMeters.Num();
		OutMesh.VerticesMeters.Append({
			Transform(FVector(-X, -Y, -Z)), Transform(FVector(X, -Y, -Z)),
			Transform(FVector(X, Y, -Z)), Transform(FVector(-X, Y, -Z)),
			Transform(FVector(-X, -Y, Z)), Transform(FVector(X, -Y, Z)),
			Transform(FVector(X, Y, Z)), Transform(FVector(-X, Y, Z)) });
		AppendIndexedCollisionQuad(OutMesh,
			BaseVertex + 0, BaseVertex + 3, BaseVertex + 2, BaseVertex + 1);
		AppendIndexedCollisionQuad(OutMesh,
			BaseVertex + 4, BaseVertex + 5, BaseVertex + 6, BaseVertex + 7);
		AppendIndexedCollisionQuad(OutMesh,
			BaseVertex + 0, BaseVertex + 1, BaseVertex + 5, BaseVertex + 4);
		AppendIndexedCollisionQuad(OutMesh,
			BaseVertex + 1, BaseVertex + 2, BaseVertex + 6, BaseVertex + 5);
		AppendIndexedCollisionQuad(OutMesh,
			BaseVertex + 3, BaseVertex + 7, BaseVertex + 6, BaseVertex + 2);
		AppendIndexedCollisionQuad(OutMesh,
			BaseVertex + 0, BaseVertex + 4, BaseVertex + 7, BaseVertex + 3);
	}

	void BuildMergedGridCollision(
		const FUERLTerrainConfig& Config,
		const FVector& PatchOrigin,
		double GridWidth,
		int32 NumX,
		int32 NumY,
		const TArray<double>& Heights,
		const TArray<uint8>& Present,
		FUERLTerrainMeshSpec& OutMesh)
	{
		check(Heights.Num() == NumX * NumY);
		check(Present.Num() == NumX * NumY);
		const double PatchSizeX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
		const double PatchSizeY = Config.CellSize[1] + 2.0 * Config.BorderWidth;
		const auto Index = [NumX](int32 X, int32 Y)
		{
			return Y * NumX + X;
		};
		const auto Top = [&Heights, &Present, &Index](int32 X, int32 Y)
		{
			return Present[Index(X, Y)] ? FMath::Max(0.0, Heights[Index(X, Y)]) : 0.0;
		};
		TArray<int32> TopVertexIndices;
		TopVertexIndices.Init(INDEX_NONE, NumX * NumY * 4);
		const auto TopVertex = [&TopVertexIndices, &Index](int32 X, int32 Y, int32 Corner)
		{
			return TopVertexIndices[Index(X, Y) * 4 + Corner];
		};
		TArray<int32> BoundaryVertexIndices;
		BoundaryVertexIndices.Init(INDEX_NONE, (NumX + 1) * (NumY + 1));
		const auto BoundaryVertex = [
			&OutMesh, &BoundaryVertexIndices, &PatchOrigin, PatchSizeX, PatchSizeY, GridWidth, NumX](
			int32 X, int32 Y)
		{
			const int32 BoundaryIndex = Y * (NumX + 1) + X;
			int32& VertexIndex = BoundaryVertexIndices[BoundaryIndex];
			if (VertexIndex == INDEX_NONE)
			{
				VertexIndex = OutMesh.VerticesMeters.Num();
				OutMesh.VerticesMeters.Add(PatchOrigin + FVector(
					-PatchSizeX * 0.5 + X * GridWidth,
					-PatchSizeY * 0.5 + Y * GridWidth,
					0.0));
			}
			return VertexIndex;
		};

		for (int32 Y = 0; Y < NumY; ++Y)
		{
			const double Y0 = -PatchSizeY * 0.5 + Y * GridWidth;
			const double Y1 = Y0 + GridWidth;
			for (int32 X = 0; X < NumX; ++X)
			{
				if (!Present[Index(X, Y)])
				{
					continue;
				}
				const double X0 = -PatchSizeX * 0.5 + X * GridWidth;
				const double X1 = X0 + GridWidth;
				const double OwnTop = Top(X, Y);
				const int32 CellBaseVertex = OutMesh.VerticesMeters.Num();
				TopVertexIndices[Index(X, Y) * 4 + 0] = CellBaseVertex;
				TopVertexIndices[Index(X, Y) * 4 + 1] = CellBaseVertex + 1;
				TopVertexIndices[Index(X, Y) * 4 + 2] = CellBaseVertex + 2;
				TopVertexIndices[Index(X, Y) * 4 + 3] = CellBaseVertex + 3;
				OutMesh.VerticesMeters.Append({
					PatchOrigin + FVector(X0, Y0, OwnTop),
					PatchOrigin + FVector(X1, Y0, OwnTop),
					PatchOrigin + FVector(X1, Y1, OwnTop),
					PatchOrigin + FVector(X0, Y1, OwnTop) });
			}
		}

		for (int32 Y = 0; Y < NumY; ++Y)
		{
			for (int32 X = 0; X < NumX; ++X)
			{
				if (!Present[Index(X, Y)])
				{
					continue;
				}
				const double OwnTop = Top(X, Y);
				AppendIndexedCollisionQuad(OutMesh,
					TopVertex(X, Y, 0), TopVertex(X, Y, 1),
					TopVertex(X, Y, 2), TopVertex(X, Y, 3));

				const bool bLeftExposed = X == 0 || !Present[Index(X - 1, Y)]
					|| Top(X - 1, Y) + KINDA_SMALL_NUMBER < OwnTop;
				if (bLeftExposed)
				{
					const bool bBoundary = X == 0 || !Present[Index(X - 1, Y)];
					AppendIndexedCollisionQuad(OutMesh,
						bBoundary ? BoundaryVertex(X, Y) : TopVertex(X - 1, Y, 1),
						TopVertex(X, Y, 0), TopVertex(X, Y, 3),
						bBoundary ? BoundaryVertex(X, Y + 1) : TopVertex(X - 1, Y, 2));
				}
				const bool bRightExposed = X == NumX - 1 || !Present[Index(X + 1, Y)]
					|| Top(X + 1, Y) + KINDA_SMALL_NUMBER < OwnTop;
				if (bRightExposed)
				{
					const bool bBoundary = X == NumX - 1 || !Present[Index(X + 1, Y)];
					AppendIndexedCollisionQuad(OutMesh,
						bBoundary ? BoundaryVertex(X + 1, Y) : TopVertex(X + 1, Y, 0),
						bBoundary ? BoundaryVertex(X + 1, Y + 1) : TopVertex(X + 1, Y, 3),
						TopVertex(X, Y, 2), TopVertex(X, Y, 1));
				}
				const bool bBackExposed = Y == 0 || !Present[Index(X, Y - 1)]
					|| Top(X, Y - 1) + KINDA_SMALL_NUMBER < OwnTop;
				if (bBackExposed)
				{
					const bool bBoundary = Y == 0 || !Present[Index(X, Y - 1)];
					AppendIndexedCollisionQuad(OutMesh,
						bBoundary ? BoundaryVertex(X, Y) : TopVertex(X, Y - 1, 3),
						bBoundary ? BoundaryVertex(X + 1, Y) : TopVertex(X, Y - 1, 2),
						TopVertex(X, Y, 1), TopVertex(X, Y, 0));
				}
				const bool bFrontExposed = Y == NumY - 1 || !Present[Index(X, Y + 1)]
					|| Top(X, Y + 1) + KINDA_SMALL_NUMBER < OwnTop;
				if (bFrontExposed)
				{
					const bool bBoundary = Y == NumY - 1 || !Present[Index(X, Y + 1)];
					AppendIndexedCollisionQuad(OutMesh,
						bBoundary ? BoundaryVertex(X, Y + 1) : TopVertex(X, Y + 1, 0),
						TopVertex(X, Y, 3), TopVertex(X, Y, 2),
						bBoundary ? BoundaryVertex(X + 1, Y + 1) : TopVertex(X, Y + 1, 1));
				}
			}
		}
	}

	bool BuildHeightfieldPatch(
		const FUERLTerrainConfig& Config,
		const FUERLTerrainTierConfig& Tier,
		const FHeightfieldParameters& Parameters,
		int32 SlotId,
		const FVector& PatchOrigin,
		TArray<FVector2D>& ResetCenters,
		TArray<double>& OutResetHeights,
		FUERLTerrainMeshSpec& OutMesh,
		FString& OutError)
	{
		const double PatchSizeX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
		const double PatchSizeY = Config.CellSize[1] + 2.0 * Config.BorderWidth;

		// Fine output grid resolves at horizontal_scale; the coarse grid is used to
		// keep neighboring vertices correlated. Without Perlin fields this is the
		// Isaac Lab random_uniform_terrain construction: discrete random heights on
		// a coarse lattice, interpolated into a continuous heightfield, then
		// quantised only at the final vertical resolution.
		const int32 NumX = FMath::Max(2, FMath::CeilToInt(PatchSizeX / Parameters.HorizontalScale) + 1);
		const int32 NumY = FMath::Max(2, FMath::CeilToInt(PatchSizeY / Parameters.HorizontalScale) + 1);
		const int32 CoarseX = FMath::Max(2, FMath::CeilToInt(PatchSizeX / Parameters.SampleScale) + 1);
		const int32 CoarseY = FMath::Max(2, FMath::CeilToInt(PatchSizeY / Parameters.SampleScale) + 1);
		const int64 VertexCount = static_cast<int64>(NumX) * NumY;
		if (VertexCount > MAX_int32 || VertexCount <= 0)
		{
			OutError = TEXT("heightfield grid is too large");
			return false;
		}
		const double StepX = PatchSizeX / static_cast<double>(NumX - 1);
		const double StepY = PatchSizeY / static_cast<double>(NumY - 1);
		const double ResetPadding = 0.5 * FMath::Max(StepX, StepY);
		// When an optional Perlin source is requested, do not anchor it at the
		// lattice origin. The origin is exactly zero for every Perlin octave,
		// which makes the reset neighbourhood look artificially flat even when
		// the patch has non-zero global relief. A seeded phase keeps that legacy
		// source reproducible while giving every Slot a genuinely random local
		// view. The default random-uniform path below does not use these phases.
		const double PerlinPhaseX = UnitSample(Tier.Seed ^ 0x517cc1b727220a95ULL, SlotId, 17, 29);
		const double PerlinPhaseY = UnitSample(Tier.Seed ^ 0x6c8e9cf570932bd5ULL, SlotId, 31, 47);
		// Deterministically sample heights on the coarse grid, quantised to
		// noise_step here. Quantising before interpolation is what keeps the
		// interpolated surface smooth; quantising afterwards would re-slice it
		// into visible steps.
		TArray<double> Coarse;
		Coarse.SetNumZeroed(CoarseX * CoarseY);
		for (int32 Cy = 0; Cy < CoarseY; ++Cy)
		{
			for (int32 Cx = 0; Cx < CoarseX; ++Cx)
			{
				const double SampleX = -PatchSizeX * 0.5
					+ static_cast<double>(Cx) * PatchSizeX / static_cast<double>(CoarseX - 1);
				const double SampleY = -PatchSizeY * 0.5
					+ static_cast<double>(Cy) * PatchSizeY / static_cast<double>(CoarseY - 1);
				double Unit = UnitSample(Tier.Seed, SlotId, Cx, Cy);
				if (Parameters.bUsePerlin)
				{
					Unit = PerlinUnit(
						Parameters, Tier.Seed, SlotId, SampleX, SampleY, PerlinPhaseX, PerlinPhaseY);
				}
				const double Raw = Parameters.NoiseRange.X
					+ Unit * (Parameters.NoiseRange.Y - Parameters.NoiseRange.X);
				Coarse[Cy * CoarseX + Cx] = Quantize(Raw, Parameters.NoiseStep);
			}
		}

		TArray<double> Heights;
		Heights.SetNumZeroed(static_cast<int32>(VertexCount));
		OutMesh.VerticesMeters.Reserve(static_cast<int32>(VertexCount));
		OutMesh.Normals.Reserve(static_cast<int32>(VertexCount));
		for (int32 Y = 0; Y < NumY; ++Y)
		{
			const double LocalY = -PatchSizeY * 0.5 + Y * StepY;
			// Map this fine row onto the coarse grid in fractional coordinates.
			const double Fy = (NumY <= 1) ? 0.0 : (static_cast<double>(Y) / (NumY - 1)) * (CoarseY - 1);
			const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Fy), 0, CoarseY - 1);
			const int32 Y1 = FMath::Min(Y0 + 1, CoarseY - 1);
			const double Ty = SmoothStepWeight(Fy - Y0);
			for (int32 X = 0; X < NumX; ++X)
			{
				const double LocalX = -PatchSizeX * 0.5 + X * StepX;
				const double Fx = (NumX <= 1) ? 0.0 : (static_cast<double>(X) / (NumX - 1)) * (CoarseX - 1);
				const int32 X0 = FMath::Clamp(FMath::FloorToInt(Fx), 0, CoarseX - 1);
				const int32 X1 = FMath::Min(X0 + 1, CoarseX - 1);
				const double Tx = SmoothStepWeight(Fx - X0);
				// Bilinear blend of the four surrounding coarse samples. Before the
				// final vertical quantisation, smoothstep weights make the surface C1
				// continuous, so no creases appear along coarse grid lines and there
				// is no height overshoot. Quantisation is only one vertical step.
				const double H00 = Coarse[Y0 * CoarseX + X0];
				const double H10 = Coarse[Y0 * CoarseX + X1];
				const double H01 = Coarse[Y1 * CoarseX + X0];
				const double H11 = Coarse[Y1 * CoarseX + X1];
				const double Top = FMath::Lerp(H00, H10, Tx);
				const double Bottom = FMath::Lerp(H01, H11, Tx);
				double Height = FMath::Lerp(Top, Bottom, Ty);
				const double DistanceToPatchEdge = FMath::Min(
					PatchSizeX * 0.5 - FMath::Abs(LocalX),
					PatchSizeY * 0.5 - FMath::Abs(LocalY));
				const double EdgeFade = Config.BorderWidth > 0.0
					? FMath::Clamp(DistanceToPatchEdge / Config.BorderWidth, 0.0, 1.0)
					: 1.0;
				Height = Quantize(Height * EdgeFade, Parameters.VerticalScale);
				const int32 Index = Y * NumX + X;
				Heights[Index] = Height;
				OutMesh.VerticesMeters.Add(PatchOrigin + FVector(LocalX, LocalY, Height));
			}
		}

		// A heightfield reset must start on a locally stable part of the actual
		// surface. Isolated Slots have one independent reset center, so search a
		// deterministic neighbourhood and choose the fine-grid candidate with the
		// smallest height range over the configured reset footprint. This changes
		// only the recorded spawn sample; the generated mesh remains untouched.
		if (SlotId != INDEX_NONE && ResetCenters.Num() == 1 && Tier.PlatformWidth > 0.0)
		{
			const double HalfWidth = Tier.PlatformWidth * 0.5 + ResetPadding;
			const double SearchRadius = FMath::Max(2.0, Tier.PlatformWidth * 2.0);
			const FVector2D RequestedCenter = ResetCenters[0];
			const double MinimumX = -PatchSizeX * 0.5 + HalfWidth;
			const double MaximumX = PatchSizeX * 0.5 - HalfWidth;
			const double MinimumY = -PatchSizeY * 0.5 + HalfWidth;
			const double MaximumY = PatchSizeY * 0.5 - HalfWidth;
			FVector2D BestCenter = RequestedCenter;
			double BestRange = TNumericLimits<double>::Max();
			double BestDistanceSquared = TNumericLimits<double>::Max();
			for (int32 CandidateY = 0; CandidateY < NumY; ++CandidateY)
			{
				const double CandidateLocalY = -PatchSizeY * 0.5 + CandidateY * StepY;
				if (FMath::Abs(CandidateLocalY - RequestedCenter.Y) > SearchRadius
					|| CandidateLocalY < MinimumY || CandidateLocalY > MaximumY)
				{
					continue;
				}
				for (int32 CandidateX = 0; CandidateX < NumX; ++CandidateX)
				{
					const double CandidateLocalX = -PatchSizeX * 0.5 + CandidateX * StepX;
					if (FMath::Abs(CandidateLocalX - RequestedCenter.X) > SearchRadius
						|| CandidateLocalX < MinimumX || CandidateLocalX > MaximumX)
					{
						continue;
					}
					double MinimumHeight = TNumericLimits<double>::Max();
					double MaximumHeight = TNumericLimits<double>::Lowest();
					for (int32 Y = 0; Y < NumY; ++Y)
					{
						const double LocalY = -PatchSizeY * 0.5 + Y * StepY;
						if (FMath::Abs(LocalY - CandidateLocalY) > HalfWidth)
						{
							continue;
						}
						for (int32 X = 0; X < NumX; ++X)
						{
							const double LocalX = -PatchSizeX * 0.5 + X * StepX;
							if (FMath::Abs(LocalX - CandidateLocalX) <= HalfWidth)
							{
								const double Height = Heights[Y * NumX + X];
								MinimumHeight = FMath::Min(MinimumHeight, Height);
								MaximumHeight = FMath::Max(MaximumHeight, Height);
							}
						}
					}
					const double HeightRange = MaximumHeight - MinimumHeight;
					const double DistanceSquared = FMath::Square(CandidateLocalX - RequestedCenter.X)
						+ FMath::Square(CandidateLocalY - RequestedCenter.Y);
					const bool bBetterRange = HeightRange < BestRange - 1.0e-9;
					const bool bSameRange = FMath::IsNearlyEqual(HeightRange, BestRange, 1.0e-9);
					if (bBetterRange || (bSameRange && DistanceSquared < BestDistanceSquared))
					{
						BestCenter = FVector2D(CandidateLocalX, CandidateLocalY);
						BestRange = HeightRange;
						BestDistanceSquared = DistanceSquared;
					}
				}
			}
			ResetCenters[0] = BestCenter;
		}

		OutResetHeights.Init(TNumericLimits<double>::Lowest(), ResetCenters.Num());
		for (int32 Y = 0; Y < NumY; ++Y)
		{
			const double LocalY = -PatchSizeY * 0.5 + Y * StepY;
			for (int32 X = 0; X < NumX; ++X)
			{
				const double LocalX = -PatchSizeX * 0.5 + X * StepX;
				const double Height = Heights[Y * NumX + X];
				for (int32 CandidateResetIndex = 0;
					CandidateResetIndex < ResetCenters.Num(); ++CandidateResetIndex)
				{
					const FVector2D& Center = ResetCenters[CandidateResetIndex];
					const double HalfWidth = Tier.PlatformWidth * 0.5 + ResetPadding;
					if (FMath::Abs(LocalX - Center.X) <= HalfWidth
						&& FMath::Abs(LocalY - Center.Y) <= HalfWidth)
					{
						OutResetHeights[CandidateResetIndex] = FMath::Max(
							OutResetHeights[CandidateResetIndex], Height);
					}
				}
			}
		}
		for (int32 Y = 0; Y < NumY; ++Y)
		{
			for (int32 X = 0; X < NumX; ++X)
			{
				const int32 Left = Y * NumX + FMath::Max(0, X - 1);
				const int32 Right = Y * NumX + FMath::Min(NumX - 1, X + 1);
				const int32 Down = FMath::Max(0, Y - 1) * NumX + X;
				const int32 Up = FMath::Min(NumY - 1, Y + 1) * NumX + X;
				const double Dx = (Heights[Right] - Heights[Left])
					/ ((Right == Left) ? StepX : 2.0 * StepX);
				const double Dy = (Heights[Up] - Heights[Down])
					/ ((Up == Down) ? StepY : 2.0 * StepY);
				OutMesh.Normals.Add(FVector(-Dx, -Dy, 1.0).GetSafeNormal());
			}
		}
		OutMesh.Triangles.Reserve((NumX - 1) * (NumY - 1) * 6);
		for (int32 Y = 0; Y < NumY - 1; ++Y)
		{
			for (int32 X = 0; X < NumX - 1; ++X)
			{
				const int32 A = Y * NumX + X;
				const int32 B = A + 1;
				const int32 C = A + NumX;
				const int32 D = C + 1;
				// UProceduralMeshComponent follows UE's left-handed winding
				// convention. Reverse the row-major order so the visible surface
				// and collision face upward instead of toward the ground.
				OutMesh.Triangles.Append({ A, C, B, B, C, D });
			}
		}
		return true;
	}

	bool BuildBoxes(
		const FUERLTerrainConfig& Config,
		const FUERLTerrainTierConfig& Tier,
		const FBoxesParameters& Parameters,
		int32 SlotId,
		const FVector& PatchOrigin,
		const TArray<FVector2D>& ResetCenters,
		TArray<double>& OutResetHeights,
		FUERLTerrainTierPlan& OutPlan)
	{
		OutResetHeights.Reset();
		const int32 BoxStart = OutPlan.Boxes.Num();
		if (Parameters.bExplicit)
		{
			for (const FUERLTerrainBoxSpec& LocalBox : Parameters.ExplicitBoxes)
			{
				FUERLTerrainBoxSpec& Box = OutPlan.Boxes.AddDefaulted_GetRef();
				Box = LocalBox;
				Box.SlotId = SlotId;
				Box.CenterMeters += PatchOrigin;
			}
			FUERLTerrainMeshSpec& CollisionMesh = OutPlan.CollisionMeshes.AddDefaulted_GetRef();
			CollisionMesh.SlotId = SlotId;
			for (int32 BoxIndex = BoxStart; BoxIndex < OutPlan.Boxes.Num(); ++BoxIndex)
			{
				AppendSolidBoxCollision(OutPlan.Boxes[BoxIndex], CollisionMesh);
			}
			return true;
		}

		const double PatchSizeX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
		const double PatchSizeY = Config.CellSize[1] + 2.0 * Config.BorderWidth;
		const int32 NumX = FMath::Max(1, FMath::FloorToInt(PatchSizeX / Parameters.GridWidth));
		const int32 NumY = FMath::Max(1, FMath::FloorToInt(PatchSizeY / Parameters.GridWidth));
		TArray<double> Heights;
		Heights.SetNumZeroed(NumX * NumY);
		TArray<uint8> Present;
		Present.SetNumZeroed(NumX * NumY);
		const double PerlinPhaseX = Parameters.bUsePerlin
			? UnitSample(Tier.Seed ^ 0x517cc1b727220a95ULL, SlotId, 17, 29)
			: 0.0;
		const double PerlinPhaseY = Parameters.bUsePerlin
			? UnitSample(Tier.Seed ^ 0x6c8e9cf570932bd5ULL, SlotId, 31, 47)
			: 0.0;
		const auto SampleDiscreteHeight = [
			&Parameters, &Tier, SlotId, PerlinPhaseX, PerlinPhaseY](double SampleX, double SampleY)
		{
			const double Unit = OpenSimplex2SUnit(
				Parameters, Tier.Seed, SlotId, SampleX, SampleY, PerlinPhaseX, PerlinPhaseY);
			return Parameters.HeightRange.X
				+ Unit * (Parameters.HeightRange.Y - Parameters.HeightRange.X);
		};
		// For discrete boxes, platform_width is the reset-height sampling window.
		// It does not flatten or otherwise modify the generated raster.
		const auto IsWithinResetFootprint = [&Tier](
			double LocalX, double LocalY, const FVector2D& Center, double Padding)
		{
			const double HalfWidth = Tier.PlatformWidth * 0.5 + Padding;
			return FMath::Abs(LocalX - Center.X) <= HalfWidth
				&& FMath::Abs(LocalY - Center.Y) <= HalfWidth;
		};
		OutResetHeights.Init(FlatBoxHeightMetres, ResetCenters.Num());
		for (int32 Y = 0; Y < NumY; ++Y)
		{
			const double LocalY = -PatchSizeY * 0.5 + (Y + 0.5) * Parameters.GridWidth;
			for (int32 X = 0; X < NumX; ++X)
			{
				const double LocalX = -PatchSizeX * 0.5 + (X + 0.5) * Parameters.GridWidth;
				const bool bOnCross = ResetCenters.ContainsByPredicate(
					[LocalX, LocalY, &Tier, &Parameters](const FVector2D& Center)
					{
						const double HalfWidth = Tier.PlatformWidth * 0.5 + Parameters.GridWidth * 0.5;
						return FMath::Abs(LocalX - Center.X) <= HalfWidth
							|| FMath::Abs(LocalY - Center.Y) <= HalfWidth;
					});
				if (Parameters.bHoles && !bOnCross)
				{
					continue;
				}
				double Height = 0.0;
				if (Parameters.bUseRandomGrid)
				{
					// Isaac Lab random_grid_terrain resolves one difficulty-dependent
					// height span, then samples each grid cell independently. UE keeps
					// the established non-negative absolute-top contract by shifting the
					// lower end to the patch datum instead of adding a centre platform.
					const double GridHeight = Parameters.HeightRange.X
						+ Parameters.Difficulty
						* (Parameters.HeightRange.Y - Parameters.HeightRange.X);
					Height = Parameters.HeightRange.X
						+ UnitSample(Tier.Seed, SlotId, X, Y)
						* (GridHeight - Parameters.HeightRange.X);
				}
				else if (Parameters.bUsePerlin)
				{
					// One cell-centre sample is the canonical discrete raster value.
					// It preserves spatial correlation without fitting a continuous
					// surface and avoids four extra noise evaluations per box.
					Height = SampleDiscreteHeight(LocalX, LocalY);
					const double DistanceToPatchEdge = FMath::Min(
						PatchSizeX * 0.5 - FMath::Abs(LocalX),
						PatchSizeY * 0.5 - FMath::Abs(LocalY));
					const double EdgeFade = Config.BorderWidth > 0.0
						? FMath::Clamp(DistanceToPatchEdge / Config.BorderWidth, 0.0, 1.0)
						: 1.0;
					Height *= EdgeFade;
				}
				else
				{
					Height = Parameters.HeightRange.X
						+ UnitSample(Tier.Seed, SlotId, X, Y)
						* (Parameters.HeightRange.Y - Parameters.HeightRange.X);
				}
				if (FMath::IsNearlyZero(Height))
				{
					Height = FlatBoxHeightMetres;
				}
				for (int32 CandidateResetIndex = 0;
					CandidateResetIndex < ResetCenters.Num(); ++CandidateResetIndex)
				{
					if (IsWithinResetFootprint(
						LocalX, LocalY, ResetCenters[CandidateResetIndex], Parameters.GridWidth * 0.5))
					{
						OutResetHeights[CandidateResetIndex] = FMath::Max(
							OutResetHeights[CandidateResetIndex], Height);
					}
				}
				FUERLTerrainBoxSpec& Box = OutPlan.Boxes.AddDefaulted_GetRef();
				Box.SlotId = SlotId;
				Box.CenterMeters = PatchOrigin + FVector(LocalX, LocalY, Height * 0.5);
				Box.ExtentMeters = FVector(
					Parameters.GridWidth * 0.5,
					Parameters.GridWidth * 0.5,
					FMath::Abs(Height) * 0.5);
				const int32 GridIndex = Y * NumX + X;
				Heights[GridIndex] = Height;
				Present[GridIndex] = 1;
			}
		}
		FUERLTerrainMeshSpec& CollisionMesh = OutPlan.CollisionMeshes.AddDefaulted_GetRef();
		CollisionMesh.SlotId = SlotId;
		BuildMergedGridCollision(
			Config, PatchOrigin, Parameters.GridWidth, NumX, NumY,
			Heights, Present, CollisionMesh);
		return true;
	}

	FName PrimitiveId(EUERLTerrainPrimitive Primitive)
	{
		switch (Primitive)
		{
		case EUERLTerrainPrimitive::Plane: return FName(TEXT("plane"));
		case EUERLTerrainPrimitive::Heightfield: return FName(TEXT("heightfield"));
		case EUERLTerrainPrimitive::Boxes: return FName(TEXT("boxes"));
		default: return NAME_None;
		}
	}

	bool MakeRequest(
		const FUERLTerrainConfig& Config,
		const FUERLTerrainTierConfig& Tier,
		int32 SlotId,
		const FVector& PatchOriginMetersValue,
		const TArray<FVector2D>& ResetCenters,
		FUERLSubTerrainRequest& OutRequest,
		FString& OutError)
	{
		OutRequest = FUERLSubTerrainRequest{};
		OutRequest.CellSize[0] = Config.CellSize[0];
		OutRequest.CellSize[1] = Config.CellSize[1];
		OutRequest.BorderWidth = Config.BorderWidth;
		OutRequest.PlatformWidth = Tier.PlatformWidth;
		OutRequest.Seed = Tier.Seed;
		OutRequest.SlotId = SlotId;
		OutRequest.PatchOriginMeters = PatchOriginMetersValue;
		OutRequest.ResetCenters = ResetCenters;
		OutRequest.Params = Tier.Params;
		OutRequest.Difficulty = FUERLTerrainAtlas::Difficulty(Tier.Level, Config.NumLevels);
		if (Tier.Primitive == EUERLTerrainPrimitive::Boxes && Tier.Params.IsValid())
		{
			double Authored = 0.0;
			if (Tier.Params->TryGetNumberField(TEXT("difficulty"), Authored)
				&& FMath::IsFinite(Authored) && Authored >= 0.0 && Authored <= 1.0)
			{
				OutRequest.Difficulty = Authored;
			}
		}
		OutError.Reset();
		return true;
	}

	void AppendPatchToPlan(const FUERLTerrainPatch& Patch, FUERLTerrainTierPlan& OutPlan)
	{
		OutPlan.Boxes.Append(Patch.Boxes);
		OutPlan.Meshes.Append(Patch.Meshes);
		OutPlan.CollisionMeshes.Append(Patch.CollisionMeshes);
	}

	static FUERLTerrainConfig ConfigFromRequest(const FUERLSubTerrainRequest& Request)
	{
		FUERLTerrainConfig Config;
		Config.NumLevels = 1;
		Config.CellSize[0] = Request.CellSize[0];
		Config.CellSize[1] = Request.CellSize[1];
		Config.BorderWidth = Request.BorderWidth;
		FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
		Tier.Level = 0;
		Tier.Seed = Request.Seed;
		Tier.PlatformWidth = Request.PlatformWidth;
		Tier.Params = Request.Params;
		return Config;
	}

	static FUERLTerrainTierConfig TierFromRequest(
		const FUERLSubTerrainRequest& Request,
		EUERLTerrainPrimitive Primitive)
	{
		FUERLTerrainTierConfig Tier;
		Tier.Level = 0;
		Tier.Primitive = Primitive;
		Tier.Seed = Request.Seed;
		Tier.PlatformWidth = Request.PlatformWidth;
		Tier.Params = Request.Params;
		return Tier;
	}

	static TSharedRef<FJsonObject> CopyJsonObject(const FJsonObject& Params)
	{
		TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params.Values)
		{
			Copy->SetField(Pair.Key, Pair.Value);
		}
		return Copy;
	}

	bool ValidatePlaneParams(const FJsonObject& Params, FString& OutError)
	{
		if (!HasExactKeys(CopyJsonObject(Params), {}))
		{
			OutError = TEXT("plane params must be an empty object");
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool ValidateHeightfieldParams(const FJsonObject& Params, FString& OutError)
	{
		FHeightfieldParameters Parameters;
		return ParseHeightfieldParameters(CopyJsonObject(Params), Parameters, OutError);
	}

	bool ValidateBoxesParams(const FJsonObject& Params, FString& OutError)
	{
		FBoxesParameters Parameters;
		return ParseBoxesParameters(CopyJsonObject(Params), Parameters, OutError);
	}

	bool GeneratePlanePatch(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError)
	{
		OutPatch = FUERLTerrainPatch{};
		if (!Request.Params.IsValid() || !ValidatePlaneParams(*Request.Params, OutError))
		{
			return false;
		}
		FUERLTerrainConfig Config = ConfigFromRequest(Request);
		FUERLTerrainTierPlan Plan;
		AddPlane(Config, Request.SlotId, Request.PatchOriginMeters, Plan);
		FUERLTerrainMeshSpec& CollisionMesh = Plan.CollisionMeshes.AddDefaulted_GetRef();
		CollisionMesh.SlotId = Request.SlotId;
		AppendSolidBoxCollision(Plan.Boxes.Last(), CollisionMesh);
		OutPatch.Boxes = MoveTemp(Plan.Boxes);
		OutPatch.CollisionMeshes = MoveTemp(Plan.CollisionMeshes);
		OutError.Reset();
		return true;
	}

	bool GenerateHeightfieldPatch(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError)
	{
		OutPatch = FUERLTerrainPatch{};
		FHeightfieldParameters Parameters;
		if (!ParseHeightfieldParameters(Request.Params, Parameters, OutError))
		{
			return false;
		}
		FUERLTerrainConfig Config = ConfigFromRequest(Request);
		FUERLTerrainTierConfig Tier = TierFromRequest(Request, EUERLTerrainPrimitive::Heightfield);
		FUERLTerrainMeshSpec Mesh;
		Mesh.SlotId = Request.SlotId;
		TArray<double> ResetHeights;
		TArray<FVector2D> ResetCenters = Request.ResetCenters;
		if (!BuildHeightfieldPatch(
			Config, Tier, Parameters, Request.SlotId, Request.PatchOriginMeters,
			ResetCenters, ResetHeights, Mesh, OutError))
		{
			return false;
		}
		OutPatch.Meshes.Add(MoveTemp(Mesh));
		OutPatch.ResetHeights = MoveTemp(ResetHeights);
		OutError.Reset();
		return true;
	}

	bool GenerateBoxesPatch(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError)
	{
		OutPatch = FUERLTerrainPatch{};
		FBoxesParameters Parameters;
		if (!ParseBoxesParameters(Request.Params, Parameters, OutError))
		{
			return false;
		}
		if (Parameters.bUseRandomGrid)
		{
			Parameters.Difficulty = Request.Difficulty;
		}
		FUERLTerrainConfig Config = ConfigFromRequest(Request);
		FUERLTerrainTierConfig Tier = TierFromRequest(Request, EUERLTerrainPrimitive::Boxes);
		FUERLTerrainTierPlan Plan;
		TArray<double> ResetHeights;
		if (!BuildBoxes(
			Config, Tier, Parameters, Request.SlotId, Request.PatchOriginMeters,
			Request.ResetCenters, ResetHeights, Plan))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("boxes generation failed");
			}
			return false;
		}
		OutPatch.Boxes = MoveTemp(Plan.Boxes);
		OutPatch.CollisionMeshes = MoveTemp(Plan.CollisionMeshes);
		OutPatch.ResetHeights = MoveTemp(ResetHeights);
		OutError.Reset();
		return true;
	}

}

using namespace UERLTerrainInternal;

bool FUERLTerrainAtlas::BuildPlan(
	const FUERLTerrainConfig& Config,
	const TArray<FVector>& SlotOrigins,
	TArray<FUERLTerrainTierPlan>& OutPlans,
	FString& OutError)
{
	OutPlans.Reset();
	if (!Config.IsValid())
	{
		OutError = TEXT("terrain configuration is invalid");
		return false;
	}
	if (SlotOrigins.Num() <= 0)
	{
		OutError = TEXT("terrain generation requires at least one Slot origin");
		return false;
	}

	OutPlans.Reserve(Config.Tiers.Num());
	for (const FUERLTerrainTierConfig& Tier : Config.Tiers)
	{
		FUERLTerrainTierPlan& Plan = OutPlans.AddDefaulted_GetRef();
		Plan.Level = Tier.Level;
		Plan.Samples.Reserve(SlotOrigins.Num());
		for (int32 SlotId = 0; SlotId < SlotOrigins.Num(); ++SlotId)
		{
			const FVector PatchOrigin = PatchOriginMeters(Config, Tier.Level, SlotOrigins[SlotId]);
			TArray<FVector2D> ResetCenters = { FVector2D::ZeroVector };
			FUERLTerrainSpawnSample& Sample = Plan.Samples.AddDefaulted_GetRef();
			Sample.Origin = PatchOrigin * CentimetresPerMetre;
			Sample.GroundHeight = Sample.Origin.Z;
			Sample.GroundNormal = FVector::UpVector;

			const IUERLSubTerrainGenerator* Generator =
				FindSubTerrainGenerator(PrimitiveId(Tier.Primitive));
			if (Generator == nullptr)
			{
				OutError = FString::Printf(
					TEXT("unsupported terrain primitive '%s'"),
					*PrimitiveId(Tier.Primitive).ToString());
				return false;
			}
			FUERLSubTerrainRequest Request;
			if (!MakeRequest(Config, Tier, SlotId, PatchOrigin, ResetCenters, Request, OutError))
			{
				return false;
			}
			FUERLTerrainPatch Patch;
			if (!Generator->Generate(Request, Patch, OutError))
			{
				return false;
			}
			AppendPatchToPlan(Patch, Plan);
			if (Tier.Primitive == EUERLTerrainPrimitive::Heightfield)
			{
				checkf(Patch.ResetHeights.Num() == ResetCenters.Num(),
					TEXT("heightfield reset-height count must match reset centers"));
				Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[0])
					* CentimetresPerMetre;
				Sample.Origin = (PatchOrigin + FVector(
					ResetCenters[0].X, ResetCenters[0].Y, Patch.ResetHeights[0]))
					* CentimetresPerMetre;
			}
			else if (Tier.Primitive == EUERLTerrainPrimitive::Boxes)
			{
				FBoxesParameters Parameters;
				if (!ParseBoxesParameters(Tier.Params, Parameters, OutError))
				{
					return false;
				}
				if (!Parameters.bExplicit && Tier.PlatformWidth > 0.0)
				{
					checkf(Patch.ResetHeights.Num() == ResetCenters.Num(),
						TEXT("box reset-height count must match reset centers"));
					Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[0]) * CentimetresPerMetre;
					Sample.Origin.Z = Sample.GroundHeight;
				}
			}
		}
	}
	return true;
}

bool FUERLTerrainAtlas::BuildSharedPlan(
	const FUERLTerrainConfig& Config,
	const TArray<FVector>& SlotOrigins,
	TArray<FUERLTerrainTierPlan>& OutPlans,
	FString& OutError)
{
	OutPlans.Reset();
	if (!Config.IsValid())
	{
		OutError = TEXT("terrain configuration is invalid");
		return false;
	}
	if (SlotOrigins.IsEmpty())
	{
		OutError = TEXT("shared terrain generation requires at least one Slot origin");
		return false;
	}

	for (const FVector& OriginCentimetres : SlotOrigins)
	{
		const FVector OriginMetres = OriginCentimetres / CentimetresPerMetre;
		if (FMath::Abs(OriginMetres.X) + Config.BorderWidth > Config.CellSize[0] * 0.5
			|| FMath::Abs(OriginMetres.Y) + Config.BorderWidth > Config.CellSize[1] * 0.5)
		{
			OutError = TEXT("shared terrain spawn grid does not fit inside cell_size");
			return false;
		}
	}

	const double TierStrideX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
	OutPlans.Reserve(Config.Tiers.Num());
	for (const FUERLTerrainTierConfig& Tier : Config.Tiers)
	{
		FUERLTerrainTierPlan& Plan = OutPlans.AddDefaulted_GetRef();
		Plan.Level = Tier.Level;
		const FVector PatchOrigin(Tier.Level * TierStrideX, 0.0, 0.0);
		TArray<FVector2D> ResetCenters;
		ResetCenters.Reserve(SlotOrigins.Num());
		Plan.Samples.Reserve(SlotOrigins.Num());
		for (const FVector& OriginCentimetres : SlotOrigins)
		{
			const FVector OriginMetres = OriginCentimetres / CentimetresPerMetre;
			// Shared atlas tiles are translated by PatchOrigin. Slot origins remain
			// local coordinates inside every tile, so they must not include that
			// per-level world translation when matching the reset sampling window.
			ResetCenters.Add(FVector2D(OriginMetres.X, OriginMetres.Y));
			FUERLTerrainSpawnSample& Sample = Plan.Samples.AddDefaulted_GetRef();
			Sample.Origin = (PatchOrigin + OriginMetres)
				* CentimetresPerMetre;
			Sample.GroundHeight = Sample.Origin.Z;
			Sample.GroundNormal = FVector::UpVector;
		}

			const IUERLSubTerrainGenerator* Generator =
			FindSubTerrainGenerator(PrimitiveId(Tier.Primitive));
		if (Generator == nullptr)
		{
			OutError = FString::Printf(
				TEXT("unsupported terrain primitive '%s'"),
				*PrimitiveId(Tier.Primitive).ToString());
			return false;
		}
		FUERLSubTerrainRequest Request;
		if (!MakeRequest(Config, Tier, INDEX_NONE, PatchOrigin, ResetCenters, Request, OutError))
		{
			return false;
		}
		FUERLTerrainPatch Patch;
		if (!Generator->Generate(Request, Patch, OutError))
		{
			return false;
		}
		AppendPatchToPlan(Patch, Plan);
		if (Tier.Primitive == EUERLTerrainPrimitive::Heightfield)
		{
			checkf(Patch.ResetHeights.Num() == ResetCenters.Num()
				&& Patch.ResetHeights.Num() == Plan.Samples.Num(),
				TEXT("shared heightfield reset-height count must match reset centers and samples"));
			for (int32 SlotId = 0; SlotId < Plan.Samples.Num(); ++SlotId)
			{
				FUERLTerrainSpawnSample& Sample = Plan.Samples[SlotId];
				Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[SlotId])
					* CentimetresPerMetre;
				Sample.Origin.Z = Sample.GroundHeight;
			}
		}
		else if (Tier.Primitive == EUERLTerrainPrimitive::Boxes)
		{
			FBoxesParameters Parameters;
			if (!ParseBoxesParameters(Tier.Params, Parameters, OutError))
			{
				return false;
			}
			if (!Parameters.bExplicit && Tier.PlatformWidth > 0.0)
			{
				checkf(Patch.ResetHeights.Num() == ResetCenters.Num()
					&& Patch.ResetHeights.Num() == Plan.Samples.Num(),
					TEXT("shared box reset-height count must match reset centers and samples"));
				for (int32 SlotId = 0; SlotId < Plan.Samples.Num(); ++SlotId)
				{
					FUERLTerrainSpawnSample& Sample = Plan.Samples[SlotId];
					Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[SlotId])
						* CentimetresPerMetre;
					Sample.Origin.Z = Sample.GroundHeight;
				}
			}
		}
	}
	return true;
}


double FUERLTerrainAtlas::Difficulty(int32 Level, int32 NumLevels)
{
	if (NumLevels <= 1 || Level <= 0)
	{
		return 0.0;
	}
	const int32 Clamped = FMath::Clamp(Level, 0, NumLevels - 1);
	return static_cast<double>(Clamped) / static_cast<double>(NumLevels - 1);
}

bool FUERLTerrainAtlas::ResolveOrigin(
	const FUERLTerrainConfig& Config,
	int32 Level,
	int32 Column,
	FVector& OutOriginMeters,
	FString& OutError)
{
	OutOriginMeters = FVector::ZeroVector;
	if (!Config.IsValid())
	{
		OutError = TEXT("terrain configuration is invalid");
		return false;
	}
	if (Level < 0 || Level >= Config.NumLevels || Column < 0)
	{
		OutError = TEXT("terrain atlas level or column is out of range");
		return false;
	}
	const double StrideX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
	const double StrideY = Config.CellSize[1] + 2.0 * Config.BorderWidth;
	OutOriginMeters = FVector(static_cast<double>(Level) * StrideX, static_cast<double>(Column) * StrideY, 0.0);
	OutError.Reset();
	return true;
}
