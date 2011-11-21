#include "Core.h"
#include "UnrealClasses.h"
#include "UnMesh3.h"
#include "UnMeshTypes.h"
#include "UnPackage.h"				// for checking game type
#include "UnMathTools.h"			// for FRotator to FCoords

#include "UnMaterial3.h"

#include "SkeletalMesh.h"
#include "StaticMesh.h"
#include "TypeConvert.h"

//#define DEBUG_SKELMESH		1
//#define DEBUG_STATICMESH		1


#if UNREAL3


//?? move outside?
float half2float(word h)
{
	union
	{
		float		f;
		unsigned	df;
	} f;

	int sign = (h >> 15) & 0x00000001;
	int exp  = (h >> 10) & 0x0000001F;
	int mant =  h        & 0x000003FF;

	exp  = exp + (127 - 15);
	mant = mant << 13;
	f.df = (sign << 31) | (exp << 23) | mant;
	return f.f;
}


static void UnpackNormals(const FPackedNormal SrcNormal[3], CMeshVertex &V)
{
	// tangents: convert to FVector (unpack) then cast to CVec3
	FVector Tangent = SrcNormal[0];
	FVector Normal  = SrcNormal[2];
	V.Tangent = CVT(Tangent);
	V.Normal  = CVT(Normal);
	if (SrcNormal[1].Data == 0)
	{
		// new UE3 version - this normal is not serialized and restored in vertex shader
		// LocalVertexFactory.usf, VertexFactoryGetTangentBasis() (static mesh)
		// GpuSkinVertexFactory.usf, SkinTangents() (skeletal mesh)
		cross(V.Normal, V.Tangent, V.Binormal);
		if (SrcNormal[2].GetW() == -1)
			V.Binormal.Negate();
	}
	else
	{
		// unpack Binormal
		FVector Binormal = SrcNormal[1];
		V.Binormal = CVT(Binormal);
	}
}



/*-----------------------------------------------------------------------------
	USkeletalMesh
-----------------------------------------------------------------------------*/

#define NUM_INFLUENCES_UE3			4
#define NUM_UV_SETS_UE3				4


#if NUM_INFLUENCES_UE3 != NUM_INFLUENCES
#error NUM_INFLUENCES_UE3 and NUM_INFLUENCES are not matching!
#endif

#if NUM_UV_SETS_UE3 != NUM_MESH_UV_SETS
#error NUM_UV_SETS_UE3 and NUM_MESH_UV_SETS are not matching!
#endif


// Implement constructor in cpp to avoid inlining (it's large enough).
// It's useful to declare TArray<> structures as forward declarations in header file.
USkeletalMesh3::USkeletalMesh3()
:	bHasVertexColors(false)
{}


USkeletalMesh3::~USkeletalMesh3()
{
	delete ConvertedMesh;
}


struct FSkelMeshSection3
{
	short				MaterialIndex;
	short				unk1;
	int					FirstIndex;
	int					NumTriangles;
	byte				unk2;

	friend FArchive& operator<<(FArchive &Ar, FSkelMeshSection3 &S)
	{
		if (Ar.ArVer < 215)
		{
			// UE2 fields
			short FirstIndex;
			short unk1, unk2, unk3, unk4, unk5, unk6, unk7;
			TArray<short> unk8;
			Ar << S.MaterialIndex << FirstIndex << unk1 << unk2 << unk3 << unk4 << unk5 << unk6 << S.NumTriangles;
			if (Ar.ArVer < 202) Ar << unk8;	// ArVer<202 -- from EndWar
			S.FirstIndex = FirstIndex;
			S.unk1 = 0;
			return Ar;
		}
		Ar << S.MaterialIndex << S.unk1 << S.FirstIndex;
		if (Ar.ArVer < 806)
		{
			// NumTriangles is unsigned short
			word NumTriangles;
			Ar << NumTriangles;
			S.NumTriangles = NumTriangles;
		}
		else
		{
			// NumTriangles is int
			Ar << S.NumTriangles;
		}
#if MCARTA
		if (Ar.Game == GAME_MagnaCarta && Ar.ArLicenseeVer >= 20)
		{
			int fC;
			Ar << fC;
		}
#endif // MCARTA
#if BLADENSOUL
		if (Ar.Game == GAME_BladeNSoul && Ar.ArVer >= 571) goto new_ver;
#endif
		if (Ar.ArVer >= 599)
		{
		new_ver:
			Ar << S.unk2;
		}
		return Ar;
	}
};

struct FIndexBuffer3
{
	TArray<word>		Indices;

	friend FArchive& operator<<(FArchive &Ar, FIndexBuffer3 &I)
	{
		int unk;						// Revision?
		Ar << RAW_ARRAY(I.Indices);
		if (Ar.ArVer < 297) Ar << unk;	// at older version compatible with FRawIndexBuffer
		return Ar;
	}
};

// real name (from Android version): FRawStaticIndexBuffer
struct FSkelIndexBuffer3				// differs from FIndexBuffer3 since version 806 - has ability to store int indices
{
	TArray<word>		Indices;

	friend FArchive& operator<<(FArchive &Ar, FSkelIndexBuffer3 &I)
	{
		guard(FSkelIndexBuffer3<<);

		if (Ar.ArVer >= 806)
		{
			int		f0;
			byte	ItemSize;
			Ar << f0 << ItemSize;
			assert(ItemSize == 2);
		}
		Ar << RAW_ARRAY(I.Indices);		//!! should use TArray<int> if ItemSize != 2 -- this will require updating UE2 code (Face.iWedge) too!
		int unk;
		if (Ar.ArVer < 297) Ar << unk;	// at older version compatible with FRawIndexBuffer

		return Ar;

		unguard;
	}
};

static bool CompareCompNormals(const FPackedNormal &N1, const FPackedNormal &N2)
{
	int Normal1 = N1.Data;
	int Normal2 = N2.Data;
	for (int i = 0; i < 3; i++)
	{
		char b1 = Normal1 & 0xFF;
		char b2 = Normal2 & 0xFF;
		if (abs(b1 - b2) > 10) return false;
		Normal1 >>= 8;
		Normal2 >>= 8;
	}
	return true;
}

struct FRigidVertex3
{
	FVector				Pos;
	FPackedNormal		Normal[3];
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];
	byte				BoneIndex;
	int					Color;

	friend FArchive& operator<<(FArchive &Ar, FRigidVertex3 &V)
	{
		int NumUVSets = 1;

#if ENDWAR
		if (Ar.Game == GAME_EndWar)
		{
			// End War uses 4-component FVector everywhere, but here it is 3-component
			Ar << V.Pos.X << V.Pos.Y << V.Pos.Z;
			goto normals;
		}
#endif // ENDWAR
		Ar << V.Pos;
#if CRIMECRAFT
		if (Ar.Game == GAME_CrimeCraft)
		{
			if (Ar.ArLicenseeVer >= 1) Ar.Seek(Ar.Tell() + sizeof(float));
			if (Ar.ArLicenseeVer >= 2) NumUVSets = 4;
		}
#endif // CRIMECRAFT
		// note: version prior 477 have different normal/tangent format (same layout, but different
		// data meaning)
	normals:
		Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
#if R6VEGAS
		if (Ar.Game == GAME_R6Vegas2 && Ar.ArLicenseeVer >= 63)
		{
			FMeshUVHalf hUV;
			Ar << hUV;
			V.UV[0] = hUV;			// convert
			goto influences;
		}
#endif // R6VEGAS

		// UVs
		if (Ar.ArVer >= 709) NumUVSets = 4;
#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 13) NumUVSets = 3;
#endif
#if MKVSDC || STRANGLE || TRANSFORMERS
		if ((Ar.Game == GAME_MK && Ar.ArLicenseeVer >= 11) || Ar.Game == GAME_Strangle ||	// Stranglehold check MidwayVer >= 17
			(Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55))
			NumUVSets = 2;
#endif
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines)
		{
			if (Ar.ArLicenseeVer >= 3 && Ar.ArLicenseeVer <= 52)	// Frontlines (the code in Homefront uses different version comparison!)
				NumUVSets = 2;
			else if (Ar.ArLicenseeVer > 52)
			{
				byte Num;
				Ar << Num;
				NumUVSets = Num;
			}
		}
#endif // FRONTLINES
		// UV
		for (int i = 0; i < NumUVSets; i++)
			Ar << V.UV[i];

		if (Ar.ArVer >= 710) Ar << V.Color;	// default 0xFFFFFFFF

#if MCARTA
		if (Ar.Game == GAME_MagnaCarta && Ar.ArLicenseeVer >= 5)
		{
			int f20;
			Ar << f20;
		}
#endif // MCARTA
	influences:
		Ar << V.BoneIndex;
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88)
		{
			int unk24;
			Ar << unk24;
		}
#endif // FRONTLINES
		return Ar;
	}
};


struct FSmoothVertex3
{
	FVector				Pos;
	FPackedNormal		Normal[3];
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];
	byte				BoneIndex[NUM_INFLUENCES_UE3];
	byte				BoneWeight[NUM_INFLUENCES_UE3];
	int					Color;

	friend FArchive& operator<<(FArchive &Ar, FSmoothVertex3 &V)
	{
		int i;
		int NumUVSets = 1;

#if ENDWAR
		if (Ar.Game == GAME_EndWar)
		{
			// End War uses 4-component FVector everywhere, but here it is 3-component
			Ar << V.Pos.X << V.Pos.Y << V.Pos.Z;
			goto normals;
		}
#endif // ENDWAR
		Ar << V.Pos;
#if CRIMECRAFT
		if (Ar.Game == GAME_CrimeCraft)
		{
			if (Ar.ArLicenseeVer >= 1) Ar.Seek(Ar.Tell() + sizeof(float));
			if (Ar.ArLicenseeVer >= 2) NumUVSets = 4;
		}
#endif // CRIMECRAFT
		// note: version prior 477 have different normal/tangent format (same layout, but different
		// data meaning)
	normals:
		Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
#if R6VEGAS
		if (Ar.Game == GAME_R6Vegas2 && Ar.ArLicenseeVer >= 63)
		{
			FMeshUVHalf hUV;
			Ar << hUV;
			V.UV[0] = hUV;			// convert
			goto influences;
		}
#endif // R6VEGAS

		// UVs
		if (Ar.ArVer >= 709) NumUVSets = 4;
#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 13) NumUVSets = 3;
#endif
#if MKVSDC || STRANGLE || TRANSFORMERS
		if ((Ar.Game == GAME_MK && Ar.ArLicenseeVer >= 11) || Ar.Game == GAME_Strangle ||	// Stranglehold check MidwayVer >= 17
			(Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55))
			NumUVSets = 2;
#endif
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines)
		{
			if (Ar.ArLicenseeVer >= 3 && Ar.ArLicenseeVer <= 52)	// Frontlines (the code in Homefront uses different version comparison!)
				NumUVSets = 2;
			else if (Ar.ArLicenseeVer > 52)
			{
				byte Num;
				Ar << Num;
				NumUVSets = Num;
			}
		}
#endif // FRONTLINES
		// UV
		for (int i = 0; i < NumUVSets; i++)
			Ar << V.UV[i];

		if (Ar.ArVer >= 710) Ar << V.Color;	// default 0xFFFFFFFF

#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88)
		{
			int unk24;
			Ar << unk24;
		}
#endif // FRONTLINES

	influences:
		if (Ar.ArVer >= 333)
		{
			for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneIndex[i];
			for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneWeight[i];
		}
		else
		{
			for (i = 0; i < NUM_INFLUENCES_UE3; i++)
				Ar << V.BoneIndex[i] << V.BoneWeight[i];
		}
#if MCARTA
		if (Ar.Game == GAME_MagnaCarta && Ar.ArLicenseeVer >= 5)
		{
			int f28;
			Ar << f28;
		}
#endif // MCARTA
		return Ar;
	}
};

#if MKVSDC
struct FMesh3Unk4_MK
{
	word				data[4];

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk4_MK &S)
	{
		return Ar << S.data[0] << S.data[1] << S.data[2] << S.data[3];
	}
};
#endif // MKVSDC

// real name: FSkelMeshChunk
struct FSkinChunk3
{
	int					FirstVertex;
	TArray<FRigidVertex3>  RigidVerts;
	TArray<FSmoothVertex3> SmoothVerts;
	TArray<short>		Bones;
	int					NumRigidVerts;
	int					NumSmoothVerts;
	int					MaxInfluences;

	friend FArchive& operator<<(FArchive &Ar, FSkinChunk3 &V)
	{
		guard(FSkinChunk3<<);
		Ar << V.FirstVertex << V.RigidVerts << V.SmoothVerts;
#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 459)
		{
			TArray<FMesh3Unk4_MK> unk1C, unk28;
			Ar << unk1C << unk28;
		}
#endif // MKVSDC
		Ar << V.Bones;
		if (Ar.ArVer >= 333)
		{
			Ar << V.NumRigidVerts << V.NumSmoothVerts;
			// note: NumRigidVerts and NumSmoothVerts may be non-zero while corresponding
			// arrays are empty - that's when GPU skin only left
		}
		else
		{
			V.NumRigidVerts  = V.RigidVerts.Num();
			V.NumSmoothVerts = V.SmoothVerts.Num();
		}
#if ARMYOF2
		if (Ar.Game == GAME_ArmyOf2 && Ar.ArLicenseeVer >= 7)
		{
			TArray<FMeshUVFloat> extraUV;
			Ar << RAW_ARRAY(extraUV);
		}
#endif // ARMYOF2
		if (Ar.ArVer >= 362)
			Ar << V.MaxInfluences;
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55)
		{
			int NumTexCoords;
			Ar << NumTexCoords;
		}
#endif // TRANSFORMERS
#if DEBUG_SKELMESH
		appPrintf("Chunk: FirstVert=%d RigidVerts=%d (%d) SmoothVerts=%d (%d)\n",
			V.FirstVertex, V.RigidVerts.Num(), V.NumRigidVerts, V.SmoothVerts.Num(), V.NumSmoothVerts);
#endif
		return Ar;
		unguard;
	}
};

struct FEdge3
{
	int					iVertex[2];
	int					iFace[2];

	friend FArchive& operator<<(FArchive &Ar, FEdge3 &V)
	{
#if BATMAN
		if (Ar.Game == GAME_Batman && Ar.ArLicenseeVer >= 5)
		{
			short iVertex[2], iFace[2];
			Ar << iVertex[0] << iVertex[1] << iFace[0] << iFace[1];
			V.iVertex[0] = iVertex[0];
			V.iVertex[1] = iVertex[1];
			V.iFace[0]   = iFace[0];
			V.iFace[1]   = iFace[1];
			return Ar;
		}
#endif // BATMAN
		return Ar << V.iVertex[0] << V.iVertex[1] << V.iFace[0] << V.iFace[1];
	}
};


// Structure holding normals and bone influeces
struct FGPUVert3Common
{
	FPackedNormal		Normal[3];
	byte				BoneIndex[NUM_INFLUENCES_UE3];
	byte				BoneWeight[NUM_INFLUENCES_UE3];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3Common &V)
	{
#if AVA
		if (Ar.Game == GAME_AVA) goto new_ver;
#endif
		if (Ar.ArVer < 494)
			Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
		else
		{
		new_ver:
			Ar << V.Normal[0] << V.Normal[2];
		}
#if CRIMECRAFT || FRONTLINES
		if ((Ar.Game == GAME_CrimeCraft && Ar.ArLicenseeVer >= 1) ||
			(Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88))
			Ar.Seek(Ar.Tell() + sizeof(float)); // pad or vertex color?
#endif
		int i;
		for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneIndex[i];
		for (i = 0; i < NUM_INFLUENCES_UE3; i++) Ar << V.BoneWeight[i];
		return Ar;
	}
};

static int GNumGPUUVSets = 1;

/*
 * Half = Float16
 * http://www.openexr.com/  source: ilmbase-*.tar.gz/Half/toFloat.cpp
 * http://en.wikipedia.org/wiki/Half_precision
 * Also look GL_ARB_half_float_pixel
 */
struct FGPUVert3Half : FGPUVert3Common
{
	FVector				Pos;
	FMeshUVHalf			UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3Half &V)
	{
		if (Ar.ArVer < 592)
			Ar << V.Pos << *((FGPUVert3Common*)&V);
		else
			Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

struct FGPUVert3Float : FGPUVert3Common
{
	FVector				Pos;
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];

	FGPUVert3Float& operator=(const FSmoothVertex3 &S)
	{
		int i;
		Pos = S.Pos;
		for (i = 0; i < NUM_MESH_UV_SETS; i++)
			UV[i] = S.UV[i];
		for (i = 0; i < NUM_INFLUENCES_UE3; i++)
		{
			BoneIndex[i]  = S.BoneIndex[i];
			BoneWeight[i] = S.BoneWeight[i];
		}
		return *this;
	}

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3Float &V)
	{
		if (Ar.ArVer < 592)
			Ar << V.Pos << *((FGPUVert3Common*)&V);
		else
			Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

//?? move to UnMeshTypes.h ?
//?? checked with Enslaved (XBox360) and MOH2010 (PC)
//?? similar to FVectorIntervalFixed32 used in animation, but has different X/Y/Z bit count
struct FVectorIntervalFixed32GPU
{
	int X:11, Y:11, Z:10;

	FVector ToVector(const FVector &Mins, const FVector &Ranges) const
	{
		FVector r;
		r.X = (X / 1023.0f) * Ranges.X + Mins.X;
		r.Y = (Y / 1023.0f) * Ranges.Y + Mins.Y;
		r.Z = (Z / 511.0f)  * Ranges.Z + Mins.Z;
		return r;
	}

	friend FArchive& operator<<(FArchive &Ar, FVectorIntervalFixed32GPU &V)
	{
		return Ar << GET_DWORD(V);
	}
};

struct FGPUVert3PackedHalf : FGPUVert3Common
{
	FVectorIntervalFixed32GPU Pos;
	FMeshUVHalf			UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3PackedHalf &V)
	{
		Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

struct FGPUVert3PackedFloat : FGPUVert3Common
{
	FVectorIntervalFixed32GPU Pos;
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert3PackedFloat &V)
	{
		Ar << *((FGPUVert3Common*)&V) << V.Pos;
		for (int i = 0; i < GNumGPUUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

// real name: FSkeletalMeshVertexBuffer
struct FGPUSkin3
{
	int							NumUVSets;
	int							bUseFullPrecisionUVs;		// 0 = half, 1 = float; copy of corresponding USkeletalMesh field
	// compressed position data
	int							bUsePackedPosition;			// 1 = packed FVector (32-bit), 0 = FVector (96-bit)
	FVector						MeshOrigin;
	FVector						MeshExtension;
	// vertex sets
	TArray<FGPUVert3Half>		VertsHalf;					// only one of these vertex sets are used
	TArray<FGPUVert3Float>		VertsFloat;
	TArray<FGPUVert3PackedHalf> VertsHalfPacked;
	TArray<FGPUVert3PackedFloat> VertsFloatPacked;

	inline int GetVertexCount() const
	{
		if (VertsHalf.Num()) return VertsHalf.Num();
		if (VertsFloat.Num()) return VertsFloat.Num();
		if (VertsHalfPacked.Num()) return VertsHalfPacked.Num();
		if (VertsFloatPacked.Num()) return VertsFloatPacked.Num();
		return 0;
	}

	friend FArchive& operator<<(FArchive &Ar, FGPUSkin3 &S)
	{
		guard(FGPUSkin3<<);

	#if DEBUG_SKELMESH
		appPrintf("Reading GPU skin\n");
	#endif
		if (Ar.IsLoading) S.bUsePackedPosition = false;
		bool AllowPackedPosition = false;
		S.NumUVSets = GNumGPUUVSets = 1;

	#if HUXLEY
		if (Ar.Game == GAME_Huxley) goto old_version;
	#endif
	#if AVA
		if (Ar.Game == GAME_AVA)
		{
			// different ArVer to check
			if (Ar.ArVer < 441) goto old_version;
			else				goto new_version;
		}
	#endif // AVA
	#if FRONTLINES
		if (Ar.Game == GAME_Frontlines)
		{
			if (Ar.ArLicenseeVer < 11)
				goto old_version;
			if (Ar.ArVer < 493 )
				S.bUseFullPrecisionUVs = true;
			else
				Ar << S.bUseFullPrecisionUVs;
			int VertexSize, NumVerts;
			Ar << S.NumUVSets << VertexSize << NumVerts;
			GNumGPUUVSets = S.NumUVSets;
			goto serialize_verts;
		}
	#endif // FRONTLINES

	#if ARMYOF2
		if (Ar.Game == GAME_ArmyOf2 && Ar.ArLicenseeVer >= 74)
		{
			int UseNewFormat;
			Ar << UseNewFormat;
			if (UseNewFormat)
			{
				appError("ArmyOfTwo: new vertex format!");	//!!!
				return Ar;
			}
		}
	#endif // ARMYOF2
		if (Ar.ArVer < 493)
		{
		old_version:
			// old version - FSmoothVertex3 array
			TArray<FSmoothVertex3> Verts;
			Ar << RAW_ARRAY(Verts);
			// convert verts
			CopyArray(S.VertsFloat, Verts);
			S.bUseFullPrecisionUVs = true;
			return Ar;
		}

		// new version
	new_version:
		// serialize type information
	#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 15) goto get_UV_count;
	#endif
	#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 55) goto get_UV_count; // 1 or 2
	#endif
		if (Ar.ArVer >= 709)
		{
		get_UV_count:
			Ar << S.NumUVSets;
			GNumGPUUVSets = S.NumUVSets;
		}
		Ar << S.bUseFullPrecisionUVs;
		if (Ar.ArVer >= 592)
			Ar << S.bUsePackedPosition << S.MeshExtension << S.MeshOrigin;

		if (Ar.Platform == PLATFORM_XBOX360 || Ar.Platform == PLATFORM_PS3) AllowPackedPosition = true;
	#if MOH2010
		if (Ar.Game == GAME_MOH2010) AllowPackedPosition = true;
	#endif
		//?? UE3 PC version ignored bUsePackedPosition - forced !bUsePackedPosition in FGPUSkin3 serializer.
		//?? Note: in UDK (newer engine) there is no code to serialize GPU vertex with packed position
		//?? working bUsePackedPosition was found in all XBox360 games and in MOH2010 (PC) only
		//?? + TRON Evolution (PS3)
#if DEBUG_SKELMESH
		appPrintf("... data: packUV:%d packVert:%d numUV:%d PackPos:(%g %g %g)+(%g %g %g)\n",
			!S.bUseFullPrecisionUVs, S.bUsePackedPosition, S.NumUVSets,
			FVECTOR_ARG(S.MeshOrigin), FVECTOR_ARG(S.MeshExtension));
#endif
		if (!AllowPackedPosition) S.bUsePackedPosition = false;		// not used in games (see comment above)

	#if CRIMECRAFT
		if (Ar.Game == GAME_CrimeCraft && Ar.ArLicenseeVer >= 2) S.NumUVSets = GNumGPUUVSets = 4;
	#endif

	serialize_verts:
		// serialize vertex array
		if (!S.bUseFullPrecisionUVs)
		{
			if (!S.bUsePackedPosition)
				Ar << RAW_ARRAY(S.VertsHalf);
			else
				Ar << RAW_ARRAY(S.VertsHalfPacked);
		}
		else
		{
			if (!S.bUsePackedPosition)
				Ar << RAW_ARRAY(S.VertsFloat);
			else
				Ar << RAW_ARRAY(S.VertsFloatPacked);
		}
#if DEBUG_SKELMESH
		appPrintf("... verts: Half[%d] HalfPacked[%d] Float[%d] FloatPacked[%d]\n",
			S.VertsHalf.Num(), S.VertsHalfPacked.Num(), S.VertsFloat.Num(), S.VertsFloatPacked.Num());
#endif

		return Ar;
		unguard;
	}
};

// real name: FVertexInfluence
struct FMesh3Unk1
{
	int					f0;
	int					f4;

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk1 &S)
	{
		return Ar << S.f0 << S.f4;
	}
};

SIMPLE_TYPE(FMesh3Unk1, int)

struct FMesh3Unk3
{
	int					f0;
	int					f4;
	TArray<word>		f8;

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk3 &S)
	{
		Ar << S.f0 << S.f4 << S.f8;
		return Ar;
	}
};

struct FMesh3Unk3A
{
	int					f0;
	int					f4;
	TArray<int>			f8;

	friend FArchive& operator<<(FArchive &Ar, FMesh3Unk3A &S)
	{
		Ar << S.f0 << S.f4 << S.f8;
		return Ar;
	}
};

struct FSkeletalMeshVertexInfluences
{
	TArray<FMesh3Unk1>	f0;
	TArray<FMesh3Unk3>	fC;				//?? Map
	TArray<FMesh3Unk3A>	fCA;
	TArray<FSkelMeshSection3> Sections;
	TArray<FSkinChunk3>	Chunks;
	TArray<byte>		f80;
	byte				f8C;			// default = 0

	friend FArchive& operator<<(FArchive &Ar, FSkeletalMeshVertexInfluences &S)
	{
		guard(FSkeletalMeshVertexInfluences<<);
		Ar << S.f0;
		if (Ar.ArVer >= 609)
		{
			if (Ar.ArVer >= 808)
			{
				Ar << S.fCA;
			}
			else
			{
				byte unk1;
				if (Ar.ArVer >= 806) Ar << unk1;
				Ar << S.fC;
			}
		}
		if (Ar.ArVer >= 700) Ar << S.Sections << S.Chunks;
		if (Ar.ArVer >= 708) Ar << S.f80;
		if (Ar.ArVer >= 715) Ar << S.f8C;
		return Ar;
		unguard;
	}
};

#if R6VEGAS

struct FMesh3R6Unk1
{
	byte				f[6];

	friend FArchive& operator<<(FArchive &Ar, FMesh3R6Unk1 &S)
	{
		Ar << S.f[0] << S.f[1] << S.f[2] << S.f[3] << S.f[4];
		if (Ar.ArLicenseeVer >= 47) Ar << S.f[5];
		return Ar;
	}
};

#endif // R6VEGAS

#if TRANSFORMERS

struct FTRMeshUnkStream
{
	int					ItemSize;
	int					NumVerts;
	TArray<int>			Data;			// TArray<FPackedNormal>

	friend FArchive& operator<<(FArchive &Ar, FTRMeshUnkStream &S)
	{
		Ar << S.ItemSize << S.NumVerts;
		if (S.ItemSize && S.NumVerts)
			Ar << RAW_ARRAY(S.Data);
		return Ar;
	}
};

#endif // TRANSFORMERS

// Version references: 180..240 - Rainbow 6: Vegas 2
// Other: GOW PC
struct FStaticLODModel3
{
	TArray<FSkelMeshSection3> Sections;
	TArray<FSkinChunk3>	Chunks;
	FSkelIndexBuffer3	IndexBuffer;
	TArray<short>		UsedBones;		// bones, value = [0, NumBones-1]
	TArray<byte>		f24;			// count = NumBones, value = [0, NumBones-1]; note: BoneIndex is 'short', not 'byte' ...
	TArray<word>		f68;			// indices, value = [0, NumVertices-1]
	TArray<byte>		f74;			// count = NumTriangles
	int					f80;
	int					NumVertices;
	TArray<FEdge3>		Edges;			// links 2 vertices and 2 faces (triangles)
	FWordBulkData		BulkData;		// ElementCount = NumVertices
	FIntBulkData		BulkData2;		// used instead of BulkData since version 806, indices?
	FGPUSkin3			GPUSkin;
	TArray<FSkeletalMeshVertexInfluences> fC4;	// GoW2+ engine
	int					NumUVSets;
	TArray<int>			VertexColor;	// since version 710

	friend FArchive& operator<<(FArchive &Ar, FStaticLODModel3 &Lod)
	{
		guard(FStaticLODModel3<<);

#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 8)
		{
			// TLazyArray-like file pointer
			int EndArPos;
			Ar << EndArPos;
		}
#endif // FURY

#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 472 && Ar.Platform == PLATFORM_PS3)
		{
			// this platform has no IndexBuffer
			Ar << Lod.Sections;
			goto part1;
		}
#endif // MKVSDC

		Ar << Lod.Sections << Lod.IndexBuffer;
#if DEBUG_SKELMESH
		for (int i1 = 0; i1 < Lod.Sections.Num(); i1++)
		{
			FSkelMeshSection3 &S = Lod.Sections[i1];
			appPrintf("Sec[%d]: M=%d, FirstIdx=%d, NumTris=%d Unk=%d\n", i1, S.MaterialIndex, S.FirstIndex, S.NumTriangles, S.unk1);
		}
		appPrintf("Indices: %d\n", Lod.IndexBuffer.Indices.Num());
#endif // DEBUG_SKELMESH

		if (Ar.ArVer < 215)
		{
			TArray<FRigidVertex3>  RigidVerts;
			TArray<FSmoothVertex3> SmoothVerts;
			Ar << SmoothVerts << RigidVerts;
			appNotify("SkeletalMesh: untested code! (ArVer=%d)", Ar.ArVer);
		}

#if ENDWAR || BORDERLANDS
		if (Ar.Game == GAME_EndWar || Ar.Game == GAME_Borderlands)
		{
			// refined field set
			Ar << Lod.UsedBones << Lod.Chunks << Lod.f80 << Lod.NumVertices;
			goto part2;
		}
#endif // ENDWAR || BORDERLANDS

#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArVer >= 536)
		{
			// Transformers: Dark of the Moon
			// refined field set + byte bone indices
			assert(Ar.ArLicenseeVer >= 152);		// has mixed version comparisons - ArVer >= 536 and ArLicenseeVer >= 152
			TArray<byte> UsedBones2;
			Ar << UsedBones2;
			CopyArray(Lod.UsedBones, UsedBones2);	// byte -> int
			Ar << Lod.Chunks << Lod.NumVertices;
			goto part2;
		}
#endif // TRANSFORMERS

	part1:
		if (Ar.ArVer < 686) Ar << Lod.f68;
		Ar << Lod.UsedBones;
		if (Ar.ArVer < 686) Ar << Lod.f74;
		if (Ar.ArVer >= 215)
		{
		chunks:
			Ar << Lod.Chunks << Lod.f80 << Lod.NumVertices;
		}
#if DEBUG_SKELMESH
		appPrintf("%d chunks, %d bones, %d verts\n", Lod.Chunks.Num(), Lod.UsedBones.Num(), Lod.NumVertices);
#endif

#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 11)
		{
			int unk84;
			Ar << unk84;	// default is 1
		}
#endif

		if (Ar.ArVer < 686) Ar << Lod.Edges;

		if (Ar.ArVer < 202)
		{
#if 0
			// old version
			TLazyArray<FVertInfluences> Influences;
			TLazyArray<FMeshWedge>      Wedges;
			TLazyArray<FMeshFace>       Faces;
			TLazyArray<FVector>         Points;
			Ar << Influences << Wedges << Faces << Points;
#else
			appError("Old UE3 FStaticLodModel");
#endif
		}

#if STRANGLE
		if (Ar.Game == GAME_Strangle)
		{
			// also check MidwayTag == "WOO " and MidwayVer >= 346
			// f24 has been moved to the end
			Lod.BulkData.Serialize(Ar);
			Ar << Lod.GPUSkin;
			Ar << Lod.f24;
			return Ar;
		}
#endif // STRANGLE

	part2:
		if (Ar.ArVer >= 207)
		{
			Ar << Lod.f24;
		}
		else
		{
			TArray<short> f24_a;
			Ar << f24_a;
		}
#if APB
		if (Ar.Game == GAME_APB)
		{
			// skip APB bulk; for details check UTexture3::Serialize()
			Ar.Seek(Ar.Tell() + 8);
			goto after_bulk;
		}
#endif // APB
		/*!! PS3 MK:
			- no Bulk here
			- int NumSections (equals to Sections.Num())
			- int 2 or 4
			- 4 x int unknown
			- int SomeSize
			- byte[SomeSize]
			- word SomeSize (same as above)
			- ...
		*/
		if (Ar.ArVer >= 221)
		{
			if (Ar.ArVer < 806)
				Lod.BulkData.Serialize(Ar);		// Bulk of word
			else
				Lod.BulkData2.Serialize(Ar);	// Bulk of int
		}
	after_bulk:
#if R6VEGAS
		if (Ar.Game == GAME_R6Vegas2 && Ar.ArLicenseeVer >= 46)
		{
			TArray<FMesh3R6Unk1> unkA0;
			Ar << unkA0;
		}
#endif // R6VEGAS
#if ARMYOF2
		if (Ar.Game == GAME_ArmyOf2 && Ar.ArLicenseeVer >= 7)
		{
			int unk84;
			TArray<FMeshUVFloat> extraUV;
			Ar << unk84 << RAW_ARRAY(extraUV);
		}
#endif // ARMYOF2
		if (Ar.ArVer >= 709)
			Ar << Lod.NumUVSets;
		else
			Lod.NumUVSets = 1;
#if MOH2010
		int RealArVer = Ar.ArVer;
		if (Ar.Game == GAME_MOH2010)
		{
			Ar.ArVer = 592;			// partially upgraded engine, change version (for easier coding)
			if (Ar.ArLicenseeVer >= 42)
				Ar << Lod.fC4;		// original code: this field is serialized after GPU Skin
		}
#endif // MOH2010
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 34)
		{
			int gpuSkinUnk;
			Ar << gpuSkinUnk;
		}
#endif // FURY
		if (Ar.ArVer >= 333)
			Ar << Lod.GPUSkin;
#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 459)
		{
			TArray<FMesh3Unk4_MK> unkA8;
			Ar << unkA8;
		}
#endif // MKVSDC
#if MOH2010
		if (Ar.Game == GAME_MOH2010)
		{
			Ar.ArVer = RealArVer;	// restore version
			if (Ar.ArLicenseeVer >= 42) return Ar;
		}
#endif
#if BLOODONSAND
		if (Ar.Game == GAME_50Cent) return Ar;	// new ArVer, but old engine
#endif
#if MEDGE
		if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 15) return Ar;	// new ArVer, but old engine
#endif // MEDGE
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 73)
		{
			FTRMeshUnkStream unkStream;
			Ar << unkStream;
			return Ar;
		}
#endif // TRANSFORMERS
		if (Ar.ArVer >= 710)
		{
			USkeletalMesh3 *LoadingMesh = (USkeletalMesh3*)UObject::GLoadingObj;
			assert(LoadingMesh);
			if (LoadingMesh->bHasVertexColors)
			{
				Ar << RAW_ARRAY(Lod.VertexColor);
				appPrintf("WARNING: SkeletalMesh %s uses vertex colors\n", LoadingMesh->Name);
			}
		}
		if (Ar.ArVer >= 534)		// post-UT3 code
			Ar << Lod.fC4;
//		assert(Lod.IndexBuffer.Indices.Num() == Lod.f68.Num()); -- mostly equals (failed in CH_TwinSouls_Cine.upk)
//		assert(Lod.BulkData.ElementCount == Lod.NumVertices); -- mostly equals (failed on some GoW packages)
		return Ar;

		unguard;
	}
};

#if A51 || MKVSDC || STRANGLE

struct FMaterialBone
{
	int					Bone;
	FName				Param;

	friend FArchive& operator<<(FArchive &Ar, FMaterialBone &V)
	{
		return Ar << V.Bone << V.Param;
	}
};

struct FSubSkeleton_MK
{
	FName				Name;
	int					BoneSet[8];			// FBoneSet

	friend FArchive& operator<<(FArchive &Ar, FSubSkeleton_MK &S)
	{
		Ar << S.Name;
		for (int i = 0; i < ARRAY_COUNT(S.BoneSet); i++) Ar << S.BoneSet[i];
		return Ar;
	}
};

struct FBoneMirrorInfo_MK
{
	int					SourceIndex;
	byte				BoneFlipAxis;		// EAxis

	friend FArchive& operator<<(FArchive &Ar, FBoneMirrorInfo_MK &M)
	{
		return Ar << M.SourceIndex << M.BoneFlipAxis;
	}
};

struct FReferenceSkeleton_MK
{
	TArray<VJointPos>	RefPose;
	TArray<short>		Parentage;
	TArray<FName>		BoneNames;
	TArray<FSubSkeleton_MK> SubSkeletons;
	TArray<FName>		UpperBoneNames;
	TArray<FBoneMirrorInfo_MK> SkelMirrorTable;
	byte				SkelMirrorAxis;		// EAxis
	byte				SkelMirrorFlipAxis;	// EAxis

	friend FArchive& operator<<(FArchive &Ar, FReferenceSkeleton_MK &S)
	{
		Ar << S.RefPose << S.Parentage << S.BoneNames;
		Ar << S.SubSkeletons;				// MidwayVer >= 56
		Ar << S.UpperBoneNames;				// MidwayVer >= 57
		Ar << S.SkelMirrorTable << S.SkelMirrorAxis << S.SkelMirrorFlipAxis;
		return Ar;
	}
};

#endif // MIDWAY ...

#if FURY
struct FSkeletalMeshLODInfoExtra
{
	int					IsForGemini;	// bool
	int					IsForTaurus;	// bool

	friend FArchive& operator<<(FArchive &Ar, FSkeletalMeshLODInfoExtra &V)
	{
		return Ar << V.IsForGemini << V.IsForTaurus;
	}
};
#endif // FURY

#if BATMAN
struct FBoneBounds
{
	int					BoneIndex;
	// FSimpleBox
	FVector				Min;
	FVector				Max;

	friend FArchive& operator<<(FArchive &Ar, FBoneBounds &B)
	{
		return Ar << B.BoneIndex << B.Min << B.Max;
	}
};
#endif // BATMAN

#if LEGENDARY

struct FSPAITag2
{
	UObject				*f0;
	int					f4;

	friend FArchive& operator<<(FArchive &Ar, FSPAITag2 &S)
	{
		Ar << S.f0;
		if (Ar.ArLicenseeVer < 10)
		{
			byte f4;
			Ar << f4;
			S.f4 = f4;
			return Ar;
		}
		int f4[9];		// serialize each bit of S.f4 as separate dword
		Ar << f4[0] << f4[1] << f4[2] << f4[3] << f4[4] << f4[5];
		if (Ar.ArLicenseeVer >= 23) Ar << f4[6];
		if (Ar.ArLicenseeVer >= 31) Ar << f4[7];
		if (Ar.ArLicenseeVer >= 34) Ar << f4[8];
		return Ar;
	}
};

#endif // LEGENDARY

void USkeletalMesh3::Serialize(FArchive &Ar)
{
	guard(USkeletalMesh3::Serialize);

#if FRONTLINES
	if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 88)
	{
		int unk320;					// default 1
		Ar << unk320;
	}
#endif // FRONTLINES

	UObject::Serialize(Ar);			// no UPrimitive ...

#if MEDGE
	if (Ar.Game == GAME_MirrorEdge && Ar.ArLicenseeVer >= 15)
	{
		int unk264;
		Ar << unk264;
	}
#endif // MEDGE
#if FURY
	if (Ar.Game == GAME_Fury)
	{
		int b1, b2, b3, b4;		// bools, serialized as ints; LoadForGemini, LoadForTaurus, IsSceneryMesh, IsPlayerMesh
		TArray<FSkeletalMeshLODInfoExtra> LODInfoExtra;
		if (Ar.ArLicenseeVer >= 14) Ar << b1 << b2;
		if (Ar.ArLicenseeVer >= 8)
		{
			Ar << b3;
			Ar << LODInfoExtra;
		}
		if (Ar.ArLicenseeVer >= 15) Ar << b4;
	}
#endif // FURY
	Ar << Bounds;
	if (Ar.ArVer < 180)
	{
		UObject *unk;
		Ar << unk;
	}
#if BATMAN
	if (Ar.Game == GAME_Batman && Ar.ArLicenseeVer >= 0x0F)
	{
		float ConservativeBounds;
		TArray<FBoneBounds> PerBoneBounds;
		Ar << ConservativeBounds << PerBoneBounds;
	}
#endif // BATMAN
	Ar << Materials;
#if BLOODONSAND
	if (Ar.Game == GAME_50Cent && Ar.ArLicenseeVer >= 65)
	{
		TArray<UObject*> OnFireMaterials;		// name is not checked
		Ar << OnFireMaterials;
	}
#endif // BLOODONSAND
#if DARKVOID
	if (Ar.Game == GAME_DarkVoid && Ar.ArLicenseeVer >= 61)
	{
		TArray<UObject*> AlternateMaterials;
		Ar << AlternateMaterials;
	}
#endif // DARKVOID
#if ALPHA_PR
	if (Ar.Game == GAME_AlphaProtocol && Ar.ArLicenseeVer >= 26)
	{
		TArray<int> SectionDepthBias;
		Ar << SectionDepthBias;
	}
#endif // ALPHA_PR
#if MKVSDC
	if (Ar.Game == GAME_MK && Ar.ArVer >= 472)	// real version is unknown
	{
		FReferenceSkeleton_MK Skel;
		Ar << Skel << SkeletalDepth;
		MeshOrigin.Set(0, 0, 0);				// not serialized
		RotOrigin.Set(0, 0, 0);
		// convert skeleton
		int NumBones = Skel.RefPose.Num();
		assert(NumBones == Skel.Parentage.Num());
		assert(NumBones == Skel.BoneNames.Num());
		RefSkeleton.Add(NumBones);
		for (int i = 0; i < NumBones; i++)
		{
			FMeshBone &B = RefSkeleton[i];
			B.Name        = Skel.BoneNames[i];
			B.BonePos     = Skel.RefPose[i];
			B.ParentIndex = Skel.Parentage[i];
//			appPrintf("BONE: [%d] %s -> %d\n", i, *B.Name, B.ParentIndex);
		}
		goto material_bones;
	}
#endif // MKVSDC
	Ar << MeshOrigin << RotOrigin;
	Ar << RefSkeleton << SkeletalDepth;
#if DEBUG_SKELMESH
	appPrintf("RefSkeleton: %d bones, %d depth\n", RefSkeleton.Num(), SkeletalDepth);
#endif
#if A51 || MKVSDC || STRANGLE
	//?? check GAME_Wheelman
	if (Ar.Engine() == GAME_MIDWAY3 && Ar.ArLicenseeVer >= 0xF)
	{
	material_bones:
		TArray<FMaterialBone> MaterialBones;
		Ar << MaterialBones;
	}
#endif // A51 || MKVSDC || STRANGLE
#if CRIMECRAFT
	if (Ar.Game == GAME_CrimeCraft && Ar.ArLicenseeVer >= 5)
	{
		byte unk8C;
		Ar << unk8C;
	}
#endif
#if LEGENDARY
	if (Ar.Game == GAME_Legendary && Ar.ArLicenseeVer >= 9)
	{
		TArray<FSPAITag2> OATStags2;
		Ar << OATStags2;
	}
#endif
	Ar << LODModels;
#if 0
	//!! also: NameIndexMap (ArVer >= 296), PerPolyKDOPs (ArVer >= 435)
#else
	DROP_REMAINING_DATA(Ar);
#endif

	guard(ConvertMesh);

	CSkeletalMesh *Mesh = new CSkeletalMesh(this);
	ConvertedMesh = Mesh;

	// convert bounds
	Mesh->BoundingSphere.R = Bounds.SphereRadius / 2;		//?? UE3 meshes has radius 2 times larger than mesh
	VectorSubtract(CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Min));
	VectorAdd     (CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Max));

	// MeshScale, MeshOrigin, RotOrigin
	VectorScale(CVT(MeshOrigin), -1, Mesh->MeshOrigin);
	Mesh->RotOrigin = RotOrigin;
	Mesh->MeshScale.Set(1, 1, 1);							// missing in UE3

	// convert LODs
	Mesh->Lods.Empty(LODModels.Num());
	assert(LODModels.Num() == LODInfo.Num());
	for (int lod = 0; lod < LODModels.Num(); lod++)
	{
		guard(ConvertLod);

		const FStaticLODModel3 &SrcLod = LODModels[lod];
		if (!SrcLod.Chunks.Num()) continue;

		int NumTexCoords = max(SrcLod.NumUVSets, SrcLod.GPUSkin.NumUVSets);		// number of texture coordinates is serialized differently for some games
		if (NumTexCoords > NUM_MESH_UV_SETS)
			appError("SkeletalMesh has %d UV sets", NumTexCoords);

		CSkelMeshLod *Lod = new (Mesh->Lods) CSkelMeshLod;
		Lod->NumTexCoords = NumTexCoords;
		Lod->HasNormals   = true;
		Lod->HasTangents  = true;

		guard(ProcessVerts);

		// get vertex count and determine vertex source
		int VertexCount = SrcLod.GPUSkin.GetVertexCount();
		bool UseGpuSkinVerts = (VertexCount > 0);
		if (!VertexCount)
		{
			const FSkinChunk3 &C = SrcLod.Chunks[SrcLod.Chunks.Num() - 1];		// last chunk
			VertexCount = C.FirstVertex + C.NumRigidVerts + C.NumSmoothVerts;
		}
		// allocate the vertices
		Lod->AllocateVerts(VertexCount);

		int chunkIndex = 0;
		const FSkinChunk3 *C = NULL;
		int lastChunkVertex = -1;
		const FGPUSkin3 &S = SrcLod.GPUSkin;
		CSkelMeshVertex *D = Lod->Verts;

		for (int Vert = 0; Vert < VertexCount; Vert++, D++)
		{
			if (Vert >= lastChunkVertex)
			{
				// proceed to next chunk
				C = &SrcLod.Chunks[chunkIndex++];
				lastChunkVertex = C->FirstVertex + C->NumRigidVerts + C->NumSmoothVerts;
			}

			if (UseGpuSkinVerts)
			{
				// NOTE: chunk may have FirstVertex set to incorrect value (for recent UE3 versions), which overlaps with the
				// previous chunk (FirstVertex=0 for a few chunks).

				// get vertex from GPU skin
				const FGPUVert3Common *V;		// has normal and influences, but no UV[] and position

				if (!S.bUseFullPrecisionUVs)
				{
					// position
					const FMeshUVHalf *SUV;
					if (!S.bUsePackedPosition)
					{
						const FGPUVert3Half &V0 = S.VertsHalf[Vert];
						D->Position = CVT(V0.Pos);
						V   = &V0;
						SUV = V0.UV;
					}
					else
					{
						const FGPUVert3PackedHalf &V0 = S.VertsHalfPacked[Vert];
						FVector VPos;
						VPos = V0.Pos.ToVector(S.MeshOrigin, S.MeshExtension);
						D->Position = CVT(VPos);
						V   = &V0;
						SUV = V0.UV;
					}
					// UV
					for (int i = 0; i < NumTexCoords; i++)
					{
						FMeshUVFloat fUV = SUV[i];				// convert
						D->UV[i] = CVT(fUV);
					}
				}
				else
				{
					// position
					const FMeshUVFloat *SUV;
					if (!S.bUsePackedPosition)
					{
						const FGPUVert3Float &V0 = S.VertsFloat[Vert];
						V = &V0;
						D->Position = CVT(V0.Pos);
						SUV = V0.UV;
					}
					else
					{
						const FGPUVert3PackedFloat &V0 = S.VertsFloatPacked[Vert];
						V = &V0;
						FVector VPos;
						VPos = V0.Pos.ToVector(S.MeshOrigin, S.MeshExtension);
						D->Position = CVT(VPos);
						SUV = V0.UV;
					}
					// UV
					for (int i = 0; i < NumTexCoords; i++)
						D->UV[i] = CVT(SUV[i]);
				}
				// convert Normal[3]
				UnpackNormals(V->Normal, *D);
				// convert influences
//				int TotalWeight = 0;
				int i2 = 0;
				for (int i = 0; i < NUM_INFLUENCES_UE3; i++)
				{
					int BoneIndex  = V->BoneIndex[i];
					int BoneWeight = V->BoneWeight[i];
					if (BoneWeight == 0) continue;				// skip this influence (but do not stop the loop!)
					D->Weight[i2] = BoneWeight / 255.0f;
					D->Bone[i2]   = C->Bones[BoneIndex];
					i2++;
//					TotalWeight += BoneWeight;
				}
//				assert(TotalWeight = 255);
				if (i2 < NUM_INFLUENCES_UE3) D->Bone[i2] = INDEX_NONE; // mark end of list
			}
			else
			{
				// old UE3 version without a GPU skin
				// get vertex from chunk
				const FMeshUVFloat *SUV;
				if (Vert < C->FirstVertex + C->NumRigidVerts)
				{
					// rigid vertex
					const FRigidVertex3 &V0 = C->RigidVerts[Vert - C->FirstVertex];
					// position and normal
					D->Position = CVT(V0.Pos);
					UnpackNormals(V0.Normal, *D);
					// single influence
					D->Weight[0] = 1.0f;
					D->Bone[0]   = C->Bones[V0.BoneIndex];
					SUV = V0.UV;
				}
				else
				{
					// smooth vertex
					const FSmoothVertex3 &V0 = C->SmoothVerts[Vert - C->FirstVertex - C->NumRigidVerts];
					// position and normal
					D->Position = CVT(V0.Pos);
					UnpackNormals(V0.Normal, *D);
					// influences
//					int TotalWeight = 0;
					int i2 = 0;
					for (int i = 0; i < NUM_INFLUENCES_UE3; i++)
					{
						int BoneIndex  = V0.BoneIndex[i];
						int BoneWeight = V0.BoneWeight[i];
						if (BoneWeight == 0) continue;
						D->Weight[i2] = BoneWeight / 255.0f;
						D->Bone[i2]   = C->Bones[BoneIndex];
						i2++;
//						TotalWeight += BoneWeight;
					}
//					assert(TotalWeight = 255);
					if (i2 < NUM_INFLUENCES_UE3) D->Bone[i2] = INDEX_NONE; // mark end of list
					SUV = V0.UV;
				}
				// UV
				for (int i = 0; i < NumTexCoords; i++)
					D->UV[i] = CVT(SUV[i]);
			}
		}

		unguard;	// ProcessVerts

		// indices
		CopyArray(Lod->Indices.Indices16, SrcLod.IndexBuffer.Indices);	//!! 16-bit only

		// sections
		guard(ProcessSections);
		Lod->Sections.Empty(SrcLod.Sections.Num());
		assert(LODModels.Num() == LODInfo.Num());
		const FSkeletalMeshLODInfo &Info = LODInfo[lod];

		for (int Sec = 0; Sec < SrcLod.Sections.Num(); Sec++)
		{
			const FSkelMeshSection3 &S = SrcLod.Sections[Sec];
			CSkelMeshSection *Dst = new (Lod->Sections) CSkelMeshSection;

			int MaterialIndex = S.MaterialIndex;
			if (MaterialIndex >= 0 && MaterialIndex < Info.LODMaterialMap.Num())
				MaterialIndex = Info.LODMaterialMap[MaterialIndex];
			Dst->Material   = (MaterialIndex < Materials.Num()) ? Materials[MaterialIndex] : NULL;
			Dst->FirstIndex = S.FirstIndex;
			Dst->NumFaces   = S.NumTriangles;
		}

		unguard;	// ProcessSections

		unguardf(("lod=%d", lod)); // ConvertLod
	}

	// copy skeleton
	guard(ProcessSkeleton);
	Mesh->RefSkeleton.Empty(RefSkeleton.Num());
	for (int i = 0; i < RefSkeleton.Num(); i++)
	{
		const FMeshBone &B = RefSkeleton[i];
		CSkelMeshBone *Dst = new (Mesh->RefSkeleton) CSkelMeshBone;
		Dst->Name        = B.Name;
		Dst->ParentIndex = B.ParentIndex;
		Dst->Position    = CVT(B.BonePos.Position);
		Dst->Orientation = CVT(B.BonePos.Orientation);
		// fix skeleton; all bones but 0
		if (i >= 1)
			Dst->Orientation.w *= -1;
	}
	unguard; // ProcessSkeleton

	unguard; // ConvertMesh

	unguard;
}


void USkeletalMesh3::PostLoad()
{
	guard(USkeletalMesh3::PostLoad);

	assert(ConvertedMesh);

	int NumSockets = Sockets.Num();
	if (NumSockets)
	{
		ConvertedMesh->Sockets.Empty(NumSockets);
		for (int i = 0; i < NumSockets; i++)
		{
			USkeletalMeshSocket *S = Sockets[i];
			if (!S) continue;
			CSkelMeshSocket *DS = new (ConvertedMesh->Sockets) CSkelMeshSocket;
			DS->Name = S->SocketName;
			DS->Bone = S->BoneName;
			CCoords &C = DS->Transform;
			C.origin = CVT(S->RelativeLocation);
			SetAxis(S->RelativeRotation, C.axis);
		}
	}

	unguard;
}


/*-----------------------------------------------------------------------------
	UAnimSet
-----------------------------------------------------------------------------*/

UAnimSet::~UAnimSet()
{
	delete ConvertedAnim;
}


// following defines will help finding new undocumented compression schemes
#define FIND_HOLES			1
//#define DEBUG_DECOMPRESS	1

static void ReadTimeArray(FArchive &Ar, int NumKeys, TArray<float> &Times, int NumFrames)
{
	guard(ReadTimeArray);
	if (NumKeys <= 1) return;

//	appPrintf("  pos=%4X keys (max=%X)[ ", Ar.Tell(), NumFrames);
	if (NumFrames < 256)
	{
		for (int k = 0; k < NumKeys; k++)
		{
			byte v;
			Ar << v;
			Times.AddItem(v);
//			if (k < 4 || k > NumKeys - 5) appPrintf(" %02X ", v);
//			else if (k == 4) appPrintf("...");
		}
	}
	else
	{
		for (int k = 0; k < NumKeys; k++)
		{
			word v;
			Ar << v;
			Times.AddItem(v);
//			if (k < 4 || k > NumKeys - 5) appPrintf(" %04X ", v);
//			else if (k == 4) appPrintf("...");
		}
	}
//	appPrintf(" ]\n");

	// align to 4 bytes
	Ar.Seek(Align(Ar.Tell(), 4));

	unguard;
}


#if TRANSFORMERS

static FQuat TransMidifyQuat(const FQuat &q, const FQuat &m)
{
	FQuat r;
	float x  = q.X, y  = q.Y, z  = q.Z, w  = q.W;
	float sx = m.X, sy = m.Y, sz = m.Z, sw = m.W;

	float VAR_A = (sy-sz)*(z-y);
	float VAR_B = (sz+sy)*(w-x);
	float VAR_C = (sw-sx)*(z+y);
	float VAR_D = (sw+sz)*(w-y) + (sw-sz)*(w+y) + (sx+sy)*(x+z);
	float xmm0  = ( VAR_D + (sx-sy)*(z-x) ) / 2;

	r.X =  (sx+sw)*(x+w) + xmm0 - VAR_D;
	r.Y = -(sw+sz)*(w-y) + xmm0 + VAR_B;
	r.Z = -(sw-sz)*(w+y) + xmm0 + VAR_C;
	r.W = -(sx+sy)*(x+z) + xmm0 + VAR_A;

	return r;
}

#endif // TRANSFORMERS


void UAnimSet::ConvertAnims()
{
	guard(UAnimSet::ConvertAnims);

	int i, j;

	CAnimSet *AnimSet = new CAnimSet(this);
	ConvertedAnim = AnimSet;

#if MASSEFF
	UBioAnimSetData *BioData = NULL;
	if ((Package->Game == GAME_MassEffect || Package->Game == GAME_MassEffect2) && !TrackBoneNames.Num() && Sequences.Num())
	{
		// Mass Effect has separated TrackBoneNames from UAnimSet to UBioAnimSetData
		BioData = Sequences[0]->m_pBioAnimSetData;
		if (BioData)
		{
			bAnimRotationOnly = BioData->bAnimRotationOnly;
			CopyArray(TrackBoneNames, BioData->TrackBoneNames);
			CopyArray(UseTranslationBoneNames, BioData->UseTranslationBoneNames);
		}
	}
#endif // MASSEFF

	CopyArray(AnimSet->TrackBoneNames, TrackBoneNames);

#if FIND_HOLES
	bool findHoles = true;
#endif
	int NumTracks = TrackBoneNames.Num();

	AnimSet->AnimRotationOnly = bAnimRotationOnly;
	if (UseTranslationBoneNames.Num())
	{
		AnimSet->UseAnimTranslation.Add(NumTracks);
		for (i = 0; i < UseTranslationBoneNames.Num(); i++)
		{
			for (j = 0; j < TrackBoneNames.Num(); j++)
				if (UseTranslationBoneNames[i] == TrackBoneNames[j])
					AnimSet->UseAnimTranslation[j] = true;
		}
	}
	if (ForceMeshTranslationBoneNames.Num())
	{
		AnimSet->ForceMeshTranslation.Add(NumTracks);
		for (i = 0; i < ForceMeshTranslationBoneNames.Num(); i++)
		{
			for (j = 0; j < TrackBoneNames.Num(); j++)
				if (ForceMeshTranslationBoneNames[i] == TrackBoneNames[j])
					AnimSet->ForceMeshTranslation[j] = true;
		}
	}

	for (i = 0; i < Sequences.Num(); i++)
	{
		const UAnimSequence *Seq = Sequences[i];
		if (!Seq)
		{
			appPrintf("WARNING: %s: no sequence %d\n", Name, i);
			continue;
		}
#if DEBUG_DECOMPRESS
		appPrintf("Sequence: %d bones, %d offsets (%g per bone), %d frames, %d compressed data\n"
			   "          trans %s, rot %s, key %s\n",
			NumTracks, Seq->CompressedTrackOffsets.Num(), Seq->CompressedTrackOffsets.Num() / (float)NumTracks,
			Seq->NumFrames,
			Seq->CompressedByteStream.Num(),
			EnumToName("AnimationCompressionFormat", Seq->TranslationCompressionFormat),
			EnumToName("AnimationCompressionFormat", Seq->RotationCompressionFormat),
			EnumToName("AnimationKeyFormat",         Seq->KeyEncodingFormat)
		);
		for (int i2 = 0; i2 < Seq->CompressedTrackOffsets.Num(); /*empty*/)
		{
			if (Seq->KeyEncodingFormat != AKF_PerTrackCompression)
			{
				int TransOffset = Seq->CompressedTrackOffsets[i2  ];
				int TransKeys   = Seq->CompressedTrackOffsets[i2+1];
				int RotOffset   = Seq->CompressedTrackOffsets[i2+2];
				int RotKeys     = Seq->CompressedTrackOffsets[i2+3];
				appPrintf("    [%d] = trans %d[%d] rot %d[%d] - %s\n", i2/4,
					TransOffset, TransKeys, RotOffset, RotKeys, *TrackBoneNames[i2/4]
				);
				i2 += 4;
			}
			else
			{
				int TransOffset = Seq->CompressedTrackOffsets[i2  ];
				int RotOffset   = Seq->CompressedTrackOffsets[i2+1];
				appPrintf("    [%d] = trans %d rot %d - %s\n", i2/2,
					TransOffset, RotOffset, *TrackBoneNames[i2/2]
				);
				i2 += 2;
			}
		}
#endif // DEBUG_DECOMPRESS
#if MASSEFF
		if (Seq->m_pBioAnimSetData != BioData)
		{
			appNotify("Mass Effect AnimSequence %s/%s has different BioAnimSetData object, removing track",
				Name, *Seq->SequenceName);
			continue;
		}
#endif // MASSEFF
		// some checks
		int offsetsPerBone = 4;
		if (Seq->KeyEncodingFormat == AKF_PerTrackCompression)
			offsetsPerBone = 2;
#if TLR
		if (Package->Game == GAME_TLR) offsetsPerBone = 6;
#endif
#if XMEN
		if (Package->Game == GAME_XMen) offsetsPerBone = 6;		// has additional CutInfo array
#endif
		if (Seq->CompressedTrackOffsets.Num() != NumTracks * offsetsPerBone && !Seq->RawAnimData.Num())
		{
			appNotify("AnimSequence %s/%s has wrong CompressedTrackOffsets size (has %d, expected %d), removing track",
				Name, *Seq->SequenceName, Seq->CompressedTrackOffsets.Num(), NumTracks * offsetsPerBone);
			continue;
		}

		// create CAnimSequence
		CAnimSequence *Dst = new (AnimSet->Sequences) CAnimSequence;
		Dst->Name      = Seq->SequenceName;
		Dst->NumFrames = Seq->NumFrames;
		Dst->Rate      = Seq->NumFrames / Seq->SequenceLength * Seq->RateScale;

		// bone tracks ...
		Dst->Tracks.Empty(NumTracks);

		FMemReader Reader(Seq->CompressedByteStream.GetData(), Seq->CompressedByteStream.Num());
		Reader.SetupFrom(*Package);

		bool hasTimeTracks = (Seq->KeyEncodingFormat == AKF_VariableKeyLerp);

		int offsetIndex = 0;
		for (j = 0; j < NumTracks; j++, offsetIndex += offsetsPerBone)
		{
			CAnimTrack *A = new (Dst->Tracks) CAnimTrack;

			int k;

			if (!Seq->CompressedTrackOffsets.Num())	//?? or if RawAnimData.Num() != 0
			{
				// using RawAnimData array
				assert(Seq->RawAnimData.Num() == NumTracks);
				CopyArray(A->KeyPos,  CVT(Seq->RawAnimData[j].PosKeys));
				CopyArray(A->KeyQuat, CVT(Seq->RawAnimData[j].RotKeys));
				CopyArray(A->KeyTime, Seq->RawAnimData[j].KeyTimes);	// may be empty
				int k;
/*				if (!A->KeyTime.Num())
				{
					int numKeys = max(A->KeyPos.Num(), A->KeyQuat.Num());
					A->KeyTime.Empty(numKeys);
					for (k = 0; k < numKeys; k++)
						A->KeyTime.AddItem(k);
				} */
				for (k = 0; k < A->KeyTime.Num(); k++)	//??
					A->KeyTime[k] *= Dst->Rate;
				continue;
			}

			FVector Mins, Ranges;	// common ...
			static const CVec3 nullVec  = { 0, 0, 0 };
			static const CQuat nullQuat = { 0, 0, 0, 1 };

// position
#define TP(Enum, VecType)						\
				case Enum:						\
					{							\
						VecType v;				\
						Reader << v;			\
						A->KeyPos.AddItem(CVT(v)); \
					}							\
					break;
// position ranged
#define TPR(Enum, VecType)						\
				case Enum:						\
					{							\
						VecType v;				\
						Reader << v;			\
						FVector v2 = v.ToVector(Mins, Ranges); \
						A->KeyPos.AddItem(CVT(v2)); \
					}							\
					break;
// rotation
#define TR(Enum, QuatType)						\
				case Enum:						\
					{							\
						QuatType q;				\
						Reader << q;			\
						A->KeyQuat.AddItem(CVT(q)); \
					}							\
					break;
// rotation ranged
#define TRR(Enum, QuatType)						\
				case Enum:						\
					{							\
						QuatType q;				\
						Reader << q;			\
						FQuat q2 = q.ToQuat(Mins, Ranges); \
						A->KeyQuat.AddItem(CVT(q2));	\
					}							\
					break;

			// decode AKF_PerTrackCompression data
			if (Seq->KeyEncodingFormat == AKF_PerTrackCompression)
			{
				// this format uses different key storage
				guard(PerTrackCompression);
				assert(Seq->TranslationCompressionFormat == ACF_Identity);
				assert(Seq->RotationCompressionFormat == ACF_Identity);

				int TransOffset = Seq->CompressedTrackOffsets[offsetIndex  ];
				int RotOffset   = Seq->CompressedTrackOffsets[offsetIndex+1];

				unsigned PackedInfo;
				AnimationCompressionFormat KeyFormat;
				int ComponentMask;
				int NumKeys;

#define DECODE_PER_TRACK_INFO(info)										\
				KeyFormat = (AnimationCompressionFormat)(info >> 28);	\
				ComponentMask = (info >> 24) & 0xF;						\
				NumKeys = info & 0xFFFFFF;								\
				hasTimeTracks = (ComponentMask & 8) != 0;

				guard(TransKeys);
				// read translation keys
				if (TransOffset == -1)
				{
					A->KeyPos.AddItem(nullVec);
#if DEBUG_DECOMPRESS
					appPrintf("    [%d] no translation data\n", j);
#endif
				}
				else
				{
					Reader.Seek(TransOffset);
					Reader << PackedInfo;
					DECODE_PER_TRACK_INFO(PackedInfo);
					A->KeyPos.Empty(NumKeys);
					if (hasTimeTracks) A->KeyPosTime.Empty(NumKeys);
#if DEBUG_DECOMPRESS
					appPrintf("    [%d] trans: fmt=%d (%s), %d keys, mask %d\n", j,
						KeyFormat, EnumToName("AnimationCompressionFormat", KeyFormat), NumKeys, ComponentMask
					);
#endif
					if (KeyFormat == ACF_IntervalFixed32NoW)
					{
						// read mins/maxs
						Mins.Set(0, 0, 0);
						Ranges.Set(0, 0, 0);
						if (ComponentMask & 1) Reader << Mins.X << Ranges.X;
						if (ComponentMask & 2) Reader << Mins.Y << Ranges.Y;
						if (ComponentMask & 4) Reader << Mins.Z << Ranges.Z;
					}
					for (k = 0; k < NumKeys; k++)
					{
						switch (KeyFormat)
						{
//						case ACF_None:
						case ACF_Float96NoW:
							{
								FVector v;
								if (ComponentMask & 7)		//?? verify this in UDK
								{
									v.Set(0, 0, 0);
									if (ComponentMask & 1) Reader << v.X;
									if (ComponentMask & 2) Reader << v.Y;
									if (ComponentMask & 4) Reader << v.Z;
								}
								else
								{
									Reader << v;
								}
								A->KeyPos.AddItem(CVT(v));
							}
							break;
						TPR(ACF_IntervalFixed32NoW, FVectorIntervalFixed32)
						case ACF_Fixed48NoW:
							{
								FVectorFixed48 v;
								v.X = v.Y = v.Z = 32767;	// corresponds to 0
								if (ComponentMask & 1) Reader << v.X;
								if (ComponentMask & 2) Reader << v.Y;
								if (ComponentMask & 4) Reader << v.Z;
								FVector v2 = v;				// convert
								float scale = 1.0f / 128;	// here vector is 128 times smaller
								v2.X *= scale;
								v2.Y *= scale;
								v2.Z *= scale;
								A->KeyPos.AddItem(CVT(v2));
							}
							break;
						case ACF_Identity:
							A->KeyPos.AddItem(nullVec);
							break;
						default:
							appError("Unknown translation compression method: %d", KeyFormat);
						}
					}
					// align to 4 bytes
					Reader.Seek(Align(Reader.Tell(), 4));
					if (hasTimeTracks)
						ReadTimeArray(Reader, NumKeys, A->KeyPosTime, Seq->NumFrames);
				}
				unguard;

				guard(RotKeys);
				// read rotation keys
				if (RotOffset == -1)
				{
					A->KeyQuat.AddItem(nullQuat);
#if DEBUG_DECOMPRESS
					appPrintf("    [%d] no rotation data\n", j);
#endif
				}
				else
				{
					Reader.Seek(RotOffset);
					Reader << PackedInfo;
					DECODE_PER_TRACK_INFO(PackedInfo);
					A->KeyQuat.Empty(NumKeys);
					if (hasTimeTracks) A->KeyQuatTime.Empty(NumKeys);
#if DEBUG_DECOMPRESS
					appPrintf("    [%d] rot  : fmt=%d (%s), %d keys, mask %d\n", j,
						KeyFormat, EnumToName("AnimationCompressionFormat", KeyFormat), NumKeys, ComponentMask
					);
#endif
					if (KeyFormat == ACF_IntervalFixed32NoW)
					{
						// read mins/maxs
						Mins.Set(0, 0, 0);
						Ranges.Set(0, 0, 0);
						if (ComponentMask & 1) Reader << Mins.X << Ranges.X;
						if (ComponentMask & 2) Reader << Mins.Y << Ranges.Y;
						if (ComponentMask & 4) Reader << Mins.Z << Ranges.Z;
					}
					for (k = 0; k < NumKeys; k++)
					{
						switch (KeyFormat)
						{
//						TR (ACF_None, FQuat)
						case ACF_Float96NoW:
							{
								FQuatFloat96NoW q;
								if (ComponentMask & 7)		//?? verify this in UDK
								{
									q.X = q.Y = q.Z = 0;
									if (ComponentMask & 1) Reader << q.X;
									if (ComponentMask & 2) Reader << q.Y;
									if (ComponentMask & 4) Reader << q.Z;
								}
								else
								{
									Reader << q;
								}
								FQuat q2 = q;				// convert
								A->KeyQuat.AddItem(CVT(q2));
							}
							break;
						case ACF_Fixed48NoW:
							{
								FQuatFixed48NoW q;
								q.X = q.Y = q.Z = 32767;	// corresponds to 0
								if (ComponentMask & 1) Reader << q.X;
								if (ComponentMask & 2) Reader << q.Y;
								if (ComponentMask & 4) Reader << q.Z;
								FQuat q2 = q;				// convert
								A->KeyQuat.AddItem(CVT(q2));
							}
							break;
						TR (ACF_Fixed32NoW, FQuatFixed32NoW)
						TRR(ACF_IntervalFixed32NoW, FQuatIntervalFixed32NoW)
						TR (ACF_Float32NoW, FQuatFloat32NoW)
						case ACF_Identity:
							A->KeyQuat.AddItem(nullQuat);
							break;
						default:
							appError("Unknown rotation compression method: %d", Seq->RotationCompressionFormat);
						}
					}
					// align to 4 bytes
					Reader.Seek(Align(Reader.Tell(), 4));
					if (hasTimeTracks)
						ReadTimeArray(Reader, NumKeys, A->KeyQuatTime, Seq->NumFrames);
				}
				unguard;

				unguard;
				continue;
				// end of AKF_PerTrackCompression block ...
			}

			// non-AKF_PerTrackCompression block

			// read animations
			int TransOffset = Seq->CompressedTrackOffsets[offsetIndex  ];
			int TransKeys   = Seq->CompressedTrackOffsets[offsetIndex+1];
			int RotOffset   = Seq->CompressedTrackOffsets[offsetIndex+2];
			int RotKeys     = Seq->CompressedTrackOffsets[offsetIndex+3];
#if TLR
			int ScaleOffset = 0, ScaleKeys = 0;
			if (Package->Game == GAME_TLR)
			{
				ScaleOffset  = Seq->CompressedTrackOffsets[offsetIndex+4];
				ScaleKeys    = Seq->CompressedTrackOffsets[offsetIndex+5];
			}
#endif // TLR
//			appPrintf("[%d:%d:%d] :  %d[%d]  %d[%d]  %d[%d]\n", j, Seq->RotationCompressionFormat, Seq->TranslationCompressionFormat, TransOffset, TransKeys, RotOffset, RotKeys, ScaleOffset, ScaleKeys);

			A->KeyPos.Empty(TransKeys);
			A->KeyQuat.Empty(RotKeys);
			if (hasTimeTracks)
			{
				A->KeyPosTime.Empty(TransKeys);
				A->KeyQuatTime.Empty(RotKeys);
			}

			// read translation keys
			if (TransKeys)
			{
#if FIND_HOLES
				int hole = TransOffset - Reader.Tell();
				if (findHoles && hole/** && abs(hole) > 4*/)	//?? should not be holes at all
				{
					appNotify("AnimSet:%s Seq:%s [%d] hole (%d) before TransTrack (KeyFormat=%d/%d)",
						Name, *Seq->SequenceName, j, hole, Seq->KeyEncodingFormat, Seq->TranslationCompressionFormat);
///					findHoles = false;
				}
#endif // FIND_HOLES
				Reader.Seek(TransOffset);
				AnimationCompressionFormat TranslationCompressionFormat = Seq->TranslationCompressionFormat;
				if (TransKeys == 1)
					TranslationCompressionFormat = ACF_None;	// single key is stored without compression
				// read mins/ranges
				if (TranslationCompressionFormat == ACF_IntervalFixed32NoW)
				{
					assert(Package->ArVer >= 761);
					Reader << Mins << Ranges;
				}
#if BORDERLANDS
				FVector Base;
				if (Package->Game == GAME_Borderlands && (TranslationCompressionFormat == ACF_Delta40NoW || TranslationCompressionFormat == ACF_Delta48NoW))
				{
					Reader << Mins << Ranges << Base;
				}
#endif // BORDERLANDS

#if TRANSFORMERS
				if (Package->Game == GAME_Transformers && TransKeys >= 4)
				{
					assert(Package->ArLicenseeVer >= 100);
					FVector Scale, Offset;
					Reader << Scale.X;
					if (Scale.X != -1)
					{
						Reader << Scale.Y << Scale.Z << Offset;
//						appPrintf("  trans: %g %g %g -- %g %g %g\n", FVECTOR_ARG(Offset), FVECTOR_ARG(Scale));
						for (k = 0; k < TransKeys; k++)
						{
							FPackedVectorTrans pos;
							Reader << pos;
							FVector pos2 = pos.ToVector(Offset, Scale); // convert
							A->KeyPos.AddItem(CVT(pos2));
						}
						goto trans_keys_done;
					} // else - original code with 4-byte overhead
				} // else - original code for uncompressed vector
#endif // TRANSFORMERS

				for (k = 0; k < TransKeys; k++)
				{
					switch (TranslationCompressionFormat)
					{
					TP (ACF_None,               FVector)
					TP (ACF_Float96NoW,         FVector)
					TPR(ACF_IntervalFixed32NoW, FVectorIntervalFixed32)
					TP (ACF_Fixed48NoW,         FVectorFixed48)
					case ACF_Identity:
						A->KeyPos.AddItem(nullVec);
						break;
#if BORDERLANDS
					case ACF_Delta48NoW:
						{
							if (k == 0)
							{
								// "Base" works as 1st key
								A->KeyPos.AddItem(CVT(Base));
								continue;
							}
							FVectorDelta48NoW V;
							Reader << V;
							FVector V2;
							V2 = V.ToVector(Mins, Ranges, Base);
							Base = V2;			// for delta
							A->KeyPos.AddItem(CVT(V2));
						}
						break;
#endif // BORDERLANDS
					default:
						appError("Unknown translation compression method: %d", Seq->TranslationCompressionFormat);
					}
				}

			trans_keys_done:
				// align to 4 bytes
				Reader.Seek(Align(Reader.Tell(), 4));
				if (hasTimeTracks)
					ReadTimeArray(Reader, TransKeys, A->KeyPosTime, Seq->NumFrames);
			}
			else
			{
//				A->KeyPos.AddItem(nullVec);
//				appNotify("No translation keys!");
			}

#if DEBUG_DECOMPRESS
			int TransEnd = Reader.Tell();
#endif
#if FIND_HOLES
			int hole = RotOffset - Reader.Tell();
			if (findHoles && hole/** && abs(hole) > 4*/)	//?? should not be holes at all
			{
				appNotify("AnimSet:%s Seq:%s [%d] hole (%d) before RotTrack (KeyFormat=%d/%d)",
					Name, *Seq->SequenceName, j, hole, Seq->KeyEncodingFormat, Seq->RotationCompressionFormat);
///				findHoles = false;
			}
#endif // FIND_HOLES
			// read rotation keys
			Reader.Seek(RotOffset);
			AnimationCompressionFormat RotationCompressionFormat = Seq->RotationCompressionFormat;
			if (RotKeys <= 0)
				goto rot_keys_done;
			if (RotKeys == 1)
			{
				RotationCompressionFormat = ACF_Float96NoW;	// single key is stored without compression
			}
			else if (RotationCompressionFormat == ACF_IntervalFixed32NoW || Package->ArVer < 761)
			{
#if SHADOWS_DAMNED
				if (Package->Game == GAME_ShadowsDamned) goto skip_ranges;
#endif
				// starting with version 761 Mins/Ranges are read only when needed - i.e. for ACF_IntervalFixed32NoW
				Reader << Mins << Ranges;
			skip_ranges: ;
			}
#if BORDERLANDS
			FQuat Base;
			if (Package->Game == GAME_Borderlands && (RotationCompressionFormat == ACF_Delta40NoW || RotationCompressionFormat == ACF_Delta48NoW))
			{
				Reader << Base;			// in addition to Mins and Ranges
			}
#endif // BORDERLANDS
#if TRANSFORMERS
			FQuat TransQuatMod;
			if (Package->Game == GAME_Transformers && RotKeys >= 2)
				Reader << TransQuatMod;
#endif // TRANSFORMERS

			for (k = 0; k < RotKeys; k++)
			{
				switch (RotationCompressionFormat)
				{
				TR (ACF_None, FQuat)
				TR (ACF_Float96NoW, FQuatFloat96NoW)
				TR (ACF_Fixed48NoW, FQuatFixed48NoW)
				TR (ACF_Fixed32NoW, FQuatFixed32NoW)
				TRR(ACF_IntervalFixed32NoW, FQuatIntervalFixed32NoW)
				TR (ACF_Float32NoW, FQuatFloat32NoW)
				case ACF_Identity:
					A->KeyQuat.AddItem(nullQuat);
					break;
#if BATMAN
				TR (ACF_Fixed48Max, FQuatFixed48Max)
#endif
#if MASSEFF
				TR (ACF_BioFixed48, FQuatBioFixed48)	// Mass Effect 2 animation compression
#endif
#if BORDERLANDS
				case ACF_Delta48NoW:
					{
						if (k == 0)
						{
							// "Base" works as 1st key
							A->KeyQuat.AddItem(CVT(Base));
							continue;
						}
						FQuatDelta48NoW q;
						Reader << q;
						FQuat q2;
						q2 = q.ToQuat(Mins, Ranges, Base);
						Base = q2;			// for delta
						A->KeyQuat.AddItem(CVT(q2));
					}
					break;
#endif // BORDERLANDS
#if TRANSFORMERS
				case ACF_IntervalFixed48NoW:
					{
						FQuatIntervalFixed48NoW q;
						FQuat q2;
						Reader << q;
						q2 = q.ToQuat(Mins, Ranges);
						q2 = TransMidifyQuat(q2, TransQuatMod);
						A->KeyQuat.AddItem(CVT(q2));
					}
					break;
#endif // TRANSFORMERS
				default:
					appError("Unknown rotation compression method: %d", Seq->RotationCompressionFormat);
				}
			}

		rot_keys_done:
			// align to 4 bytes
			Reader.Seek(Align(Reader.Tell(), 4));
			if (hasTimeTracks)
				ReadTimeArray(Reader, RotKeys, A->KeyQuatTime, Seq->NumFrames);

#if TLR
			if (ScaleKeys)
			{
				//?? no ScaleKeys support, simply drop data
				Reader.Seek(ScaleOffset + ScaleKeys * 12);
				Reader.Seek(Align(Reader.Tell(), 4));
			}
#endif // TLR

#if DEBUG_DECOMPRESS
//			appPrintf("[%s : %s] Frames=%d KeyPos.Num=%d KeyQuat.Num=%d KeyFmt=%s\n", *Seq->SequenceName, *TrackBoneNames[j],
//				Seq->NumFrames, A->KeyPos.Num(), A->KeyQuat.Num(), *Seq->KeyEncodingFormat);
			appPrintf("  ->[%d]: t %d .. %d + r %d .. %d (%d/%d keys)\n", j,
				TransOffset, TransEnd, RotOffset, Reader.Tell(), TransKeys, RotKeys);
#endif // DEBUG_DECOMPRESS
		}
	}

	unguard;
}

/*-----------------------------------------------------------------------------
	UStaticMesh
-----------------------------------------------------------------------------*/

// Implement constructor in cpp to avoid inlining (it's large enough).
// It's useful to declare TArray<> structures as forward declarations in header file.
UStaticMesh3::UStaticMesh3()
{}

UStaticMesh3::~UStaticMesh3()
{
	delete ConvertedMesh;
}

#if TRANSFORMERS

struct FTRStaticMeshSectionUnk
{
	int					f0;
	int					f4;

	friend FArchive& operator<<(FArchive &Ar, FTRStaticMeshSectionUnk &S)
	{
		return Ar << S.f0 << S.f4;
	}
};

SIMPLE_TYPE(FTRStaticMeshSectionUnk, int)

#endif // TRANSFORMERS

#if MOH2010

struct FMOHStaticMeshSectionUnk
{
	TArray<int>			f4;
	TArray<int>			f10;
	TArray<short>		f1C;
	TArray<short>		f28;
	TArray<short>		f34;
	TArray<short>		f40;
	TArray<short>		f4C;
	TArray<short>		f58;

	friend FArchive& operator<<(FArchive &Ar, FMOHStaticMeshSectionUnk &S)
	{
		return Ar << S.f4 << S.f10 << S.f1C << S.f28 << S.f34 << S.f40 << S.f4C << S.f58;
	}
};

#endif // MOH2010


struct FStaticMeshSection3
{
	UMaterialInterface	*Mat;
	int					f10;		//?? bUseSimple...Collision
	int					f14;		//?? ...
	int					bEnableShadowCasting;
	int					FirstIndex;
	int					NumFaces;
	int					f24;		//?? first used vertex
	int					f28;		//?? last used vertex
	int					Index;		//?? index of section
	TArray<FMesh3Unk1>	f30;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshSection3 &S)
	{
		guard(FStaticMeshSection3<<);
#if TUROK
		if (Ar.Game == GAME_Turok && Ar.ArLicenseeVer >= 57)
		{
			// no S.Mat
			return Ar << S.f10 << S.f14 << S.FirstIndex << S.NumFaces << S.f24 << S.f28;
		}
#endif // TUROK
		Ar << S.Mat << S.f10 << S.f14;
		if (Ar.ArVer >= 473) Ar << S.bEnableShadowCasting;
		Ar << S.FirstIndex << S.NumFaces << S.f24 << S.f28;
#if MASSEFF
		if (Ar.Game == GAME_MassEffect && Ar.ArVer >= 485)
			return Ar << S.Index;				//?? other name?
#endif // MASSEFF
#if HUXLEY
		if (Ar.Game == GAME_Huxley && Ar.ArVer >= 485)
			return Ar << S.Index;				//?? other name?
#endif // HUXLEY
		if (Ar.ArVer >= 492) Ar << S.Index;		//?? real version is unknown! This field is missing in GOW1_PC (490), but present in UT3 (512)
#if ALPHA_PR
		if (Ar.Game == GAME_AlphaProtocol && Ar.ArLicenseeVer >= 13)
		{
			int unk30, unk34;
			Ar << unk30 << unk34;
		}
#endif // ALPHA_PR
#if MKVSDC
		if (Ar.Game == GAME_MK && Ar.ArVer >= 409)
		{
			TArray<FEdge3> unk28;				// TArray<int[4]>
			Ar << unk28;
		}
#endif // MKVSDC
#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 49)
		{
			TArray<FTRStaticMeshSectionUnk> f30;
			Ar << f30;
			return Ar;
		}
#endif // TRANSFORMERS
		if (Ar.ArVer >= 514) Ar << S.f30;
#if ALPHA_PR
		if (Ar.Game == GAME_AlphaProtocol && Ar.ArLicenseeVer >= 39)
		{
			int unk38;
			Ar << unk38;
		}
#endif // ALPHA_PR
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArVer >= 575)
		{
			byte flag;
			FMOHStaticMeshSectionUnk unk3C;
			Ar << flag;
			if (flag) Ar << unk3C;
		}
#endif // MOH2010
		if (Ar.ArVer >= 618)
		{
			byte unk;
			Ar << unk;
			assert(unk == 0);
		}
		return Ar;
		unguard;
	}
};

struct FStaticMeshVertexStream3
{
	int					VertexSize;		// 0xC
	int					NumVerts;		// == Verts.Num()
	TArray<FVector>		Verts;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshVertexStream3 &S)
	{
		guard(FStaticMeshVertexStream3<<);

		Ar << S.VertexSize << S.NumVerts;
#if BATMAN
		if (Ar.Game == GAME_Batman && Ar.ArLicenseeVer >= 0x11)
		{
			int unk18;					// default is 1
			Ar << unk18;
		}
#endif // BATMAN
#if AVA
		if (Ar.Game == GAME_AVA && Ar.ArVer >= 442)
		{
			int presence;
			Ar << presence;
			if (!presence)
			{
				appNotify("AVA: StaticMesh without vertex stream");
				return Ar;
			}
		}
#endif // AVA
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 58)
		{
			int unk28;
			Ar << unk28;
		}
#endif // MOH2010
#if SHADOWS_DAMNED
		if (Ar.Game == GAME_ShadowsDamned && Ar.ArLicenseeVer >= 26)
		{
			int unk28;
			Ar << unk28;
		}
#endif // SHADOWS_DAMNED
#if DEBUG_STATICMESH
		appPrintf("StaticMesh Vertex stream: IS:%d NV:%d\n", S.VertexSize, S.NumVerts);
#endif
		Ar << RAW_ARRAY(S.Verts);
		return Ar;

		unguard;
	}
};


static int  GNumStaticUVSets   = 1;
static bool GUseStaticFloatUVs = true;

struct FStaticMeshUVItem3
{
	FVector				Pos;			// old version (< 472)
	FPackedNormal		Normal[3];
	int					f10;			//?? VertexColor?
	FMeshUVFloat		UV[NUM_MESH_UV_SETS];

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUVItem3 &V)
	{
		guard(FStaticMeshUVItem3<<);

#if MKVSDC
		if (Ar.Game == GAME_MK)
		{
			if (Ar.ArVer >= 472) goto uvs;	// normals are stored in FStaticMeshNormalStream_MK
			int unk;
			Ar << unk;
			goto new_ver;
		}
#endif // MKVSDC
#if A51
		if (Ar.Game == GAME_A51)
			goto new_ver;
#endif
#if AVA
		if (Ar.Game == GAME_AVA)
		{
			assert(Ar.ArVer >= 441);
			Ar << V.Normal[0] << V.Normal[2] << V.f10;
			goto uvs;
		}
#endif // AVA

		if (Ar.ArVer < 472)
		{
			// old version has position embedded into UVStream (this is not an UVStream, this is a single stream for everything)
			int unk10;					// pad or color ?
			Ar << V.Pos << unk10;
		}
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 7)
		{
			int fC;
			Ar << fC;					// really should be serialized before unk10 above (it's FColor)
		}
#endif // FURY
	new_ver:
		if (Ar.ArVer < 477)
			Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
		else
			Ar << V.Normal[0] << V.Normal[2];
#if APB
		if (Ar.Game == GAME_APB && Ar.ArLicenseeVer >= 12) goto uvs;
#endif
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 58) goto uvs;
#endif
#if UNDERTOW
		if (Ar.Game == GAME_Undertow) goto uvs;
#endif
		if (Ar.ArVer >= 434 && Ar.ArVer < 615)
			Ar << V.f10;				// starting from 615 made as separate stream
	uvs:
		if (GUseStaticFloatUVs)
		{
			for (int i = 0; i < GNumStaticUVSets; i++)
				Ar << V.UV[i];
		}
		else
		{
			for (int i = 0; i < GNumStaticUVSets; i++)
			{
				// read in half format and convert to float
				FMeshUVHalf UVHalf;
				Ar << UVHalf;
				V.UV[i] = UVHalf;		// convert
			}
		}
		return Ar;

		unguard;
	}
};

struct FStaticMeshUVStream3
{
	int					NumTexCoords;
	int					ItemSize;
	int					NumVerts;
	int					bUseFullPrecisionUVs;
	TArray<FStaticMeshUVItem3> UV;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUVStream3 &S)
	{
		guard(FStaticMeshUVStream3<<);

		Ar << S.NumTexCoords << S.ItemSize << S.NumVerts;
		S.bUseFullPrecisionUVs = true;
#if TUROK
		if (Ar.Game == GAME_Turok && Ar.ArLicenseeVer >= 59)
		{
			int HalfPrecision, unk28;
			Ar << HalfPrecision << unk28;
			S.bUseFullPrecisionUVs = !HalfPrecision;
			assert(S.bUseFullPrecisionUVs);
		}
#endif // TUROK
#if AVA
		if (Ar.Game == GAME_AVA && Ar.ArVer >= 441) goto new_ver;
#endif
#if MOHA
		if (Ar.Game == GAME_MOHA && Ar.ArVer >= 421) goto new_ver;
#endif
		if (Ar.ArVer >= 474)
		{
		new_ver:
			Ar << S.bUseFullPrecisionUVs;
		}
#if DEBUG_STATICMESH
		appPrintf("StaticMesh UV stream: TC:%d IS:%d NV:%d FloatUV:%d\n", S.NumTexCoords, S.ItemSize, S.NumVerts, S.bUseFullPrecisionUVs);
#endif
#if MKVSDC
		if (Ar.Game == GAME_MK)
			S.bUseFullPrecisionUVs = false;
#endif
#if A51
		if (Ar.Game == GAME_A51 && Ar.ArLicenseeVer >= 22) // or MidwayVer ?
			Ar << S.bUseFullPrecisionUVs;
#endif
#if MOH2010
		if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 58)
		{
			int unk30;
			Ar << unk30;
		}
#endif // MOH2010
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 34)
		{
			int unused;			// useless stack variable, always 0
			Ar << unused;
		}
#endif // FURY
#if SHADOWS_DAMNED
		if (Ar.Game == GAME_ShadowsDamned && Ar.ArLicenseeVer >= 22)
		{
			int unk30;
			Ar << unk30;
		}
#endif // SHADOWS_DAMNED
		// prepare for UV serialization
		if (S.NumTexCoords > NUM_MESH_UV_SETS)
			appError("StaticMesh has %d UV sets", S.NumTexCoords);
		GNumStaticUVSets   = S.NumTexCoords;
		GUseStaticFloatUVs = S.bUseFullPrecisionUVs;
		Ar << RAW_ARRAY(S.UV);
		return Ar;

		unguard;
	}
};

struct FStaticMeshColorStream3
{
	int					ItemSize;
	int					NumVerts;
	TArray<int>			Colors;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshColorStream3 &S)
	{
		guard(FStaticMeshColorStream3<<);
		return Ar << S.ItemSize << S.NumVerts << RAW_ARRAY(S.Colors);
		unguard;
	}
};

// new color stream: difference is that data array is not serialized when NumVerts is 0
struct FStaticMeshColorStream3New		// ArVer >= 615
{
	int					ItemSize;
	int					NumVerts;
	TArray<int>			Colors;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshColorStream3New &S)
	{
		guard(FStaticMeshColorStream3New<<);
		Ar << S.ItemSize << S.NumVerts;
		if (S.NumVerts) Ar << RAW_ARRAY(S.Colors);
		return Ar;
		unguard;
	}
};

struct FStaticMeshVertex3Old			// ArVer < 333
{
	FVector				Pos;
	FPackedNormal		Normal[3];		// packed vector

	operator FVector() const
	{
		return Pos;
	}

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshVertex3Old &V)
	{
		return Ar << V.Pos << V.Normal[0] << V.Normal[1] << V.Normal[2];
	}
};

struct FStaticMeshUVStream3Old			// ArVer < 364; corresponds to UE2 StaticMesh?
{
	TArray<FMeshUVFloat> Data;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUVStream3Old &S)
	{
		guard(FStaticMeshUVStream3Old<<);
		int unk;						// Revision?
		Ar << S.Data;					// used RAW_ARRAY, but RAW_ARRAY is newer than this version
		if (Ar.ArVer < 297) Ar << unk;
		return Ar;
		unguard;
	}
};

#if MKVSDC

struct FStaticMeshNormal_MK
{
	FPackedNormal		Normal[3];		// packed vector

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshNormal_MK &V)
	{
		return Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];
	}
};

struct FStaticMeshNormalStream_MK
{
	int					ItemSize;
	int					NumVerts;
	TArray<FStaticMeshNormal_MK> Normals;

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshNormalStream_MK &S)
	{
		return Ar << S.ItemSize << S.NumVerts << RAW_ARRAY(S.Normals);
	}
};

#endif // MKVSDC

struct FStaticMeshLODModel
{
	FByteBulkData		BulkData;		// ElementSize = 0xFC for UT3 and 0x170 for UDK ... it's simpler to skip it
	TArray<FStaticMeshSection3> Sections;
	FStaticMeshVertexStream3    VertexStream;
	FStaticMeshUVStream3        UVStream;
	FStaticMeshColorStream3     ColorStream;	//??
	FStaticMeshColorStream3New  ColorStream2;	//??
	FIndexBuffer3		Indices;
	FIndexBuffer3		Indices2;		// wireframe
	int					f80;
	TArray<FEdge3>		Edges;			//??
	TArray<byte>		fEC;			//?? flags for faces? removed simultaneously with Edges

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshLODModel &Lod)
	{
		guard(FStaticMeshLODModel<<);

#if DEBUG_STATICMESH
		appPrintf("Serialize UStaticMesh LOD\n");
#endif
#if FURY
		if (Ar.Game == GAME_Fury)
		{
			int EndArPos, unkD0;
			if (Ar.ArLicenseeVer >= 8)	Ar << EndArPos;			// TLazyArray-like file pointer
			if (Ar.ArLicenseeVer >= 18)	Ar << unkD0;
		}
#endif // FURY
#if HUXLEY
		if (Ar.Game == GAME_Huxley && Ar.ArLicenseeVer >= 14)
		{
			// Huxley has different IndirectArray layout: each item
			// has stored data size before data itself
			int DataSize;
			Ar << DataSize;
		}
#endif // HUXLEY

#if APB
		if (Ar.Game == GAME_APB)
		{
			// skip bulk; check UTexture3::Serialize() for details
			Ar.Seek(Ar.Tell() + 8);
			goto after_bulk;
		}
#endif // APB
		if (Ar.ArVer >= 218)
			Lod.BulkData.Skip(Ar);

	after_bulk:

#if TLR
		if (Ar.Game == GAME_TLR && Ar.ArLicenseeVer >= 2)
		{
			FByteBulkData unk128;
			unk128.Skip(Ar);
		}
#endif // TLR
		Ar << Lod.Sections;
#if DEBUG_STATICMESH
		appPrintf("%d sections\n", Lod.Sections.Num());
		for (int i = 0; i < Lod.Sections.Num(); i++)
		{
			FStaticMeshSection3 &S = Lod.Sections[i];
			appPrintf("Mat: %s\n", S.Mat ? S.Mat->Name : "?");
			appPrintf("  %d %d sh=%d i0=%d NF=%d %d %d idx=%d\n", S.f10, S.f14, S.bEnableShadowCasting, S.FirstIndex, S.NumFaces, S.f24, S.f28, S.Index);
		}
#endif // DEBUG_STATICMESH
		// serialize vertex and uv streams
#if A51
		if (Ar.Game == GAME_A51) goto new_ver;
#endif
#if MKVSDC || AVA
		if (Ar.Game == GAME_MK || Ar.Game == GAME_AVA) goto ver_3;
#endif

#if BORDERLANDS
		if (Ar.Game == GAME_Borderlands)
		{
			// refined field set
			Ar << Lod.VertexStream;
			Ar << Lod.UVStream;
			Ar << Lod.f80;
			Ar << Lod.Indices;
			// note: no fEC (smoothing groups?)
			return Ar;
		}
#endif // BORDERLANDS

#if TRANSFORMERS
		if (Ar.Game == GAME_Transformers)
		{
			// code is similar to original code (ArVer >= 472) but has different versioning and a few new fields
			FTRMeshUnkStream unkStream;		// normals?
			int unkD8;						// part of Indices2
			Ar << Lod.VertexStream << Lod.UVStream;
			if (Ar.ArVer >= 536) Ar << Lod.ColorStream2;
			if (Ar.ArLicenseeVer >= 71) Ar << unkStream;
			if (Ar.ArVer < 536) Ar << Lod.ColorStream;
			Ar << Lod.f80 << Lod.Indices << Lod.Indices2;
			if (Ar.ArLicenseeVer >= 58) Ar << unkD8;
			if (Ar.ArVer < 536)
			{
				Ar << RAW_ARRAY(Lod.Edges);
				Ar << Lod.fEC;
			}
			return Ar;
		}
#endif // TRANSFORMERS

		if (Ar.ArVer >= 472)
		{
		new_ver:
			Ar << Lod.VertexStream;
			Ar << Lod.UVStream;
#if MOH2010
			if (Ar.Game == GAME_MOH2010 && Ar.ArLicenseeVer >= 55) goto color_stream;
#endif
#if BLADENSOUL
			if (Ar.Game == GAME_BladeNSoul && Ar.ArVer >= 572) goto color_stream;
#endif
			// unknown data in UDK
			if (Ar.ArVer >= 615)
			{
			color_stream:
				Ar << Lod.ColorStream2;
			}
			if (Ar.ArVer < 686) Ar << Lod.ColorStream;	//?? probably this is not a color stream - the same version is used to remove "edges"
			Ar << Lod.f80;
		}
		else if (Ar.ArVer >= 466)
		{
		ver_3:
#if MKVSDC
			if (Ar.Game == GAME_MK && Ar.ArVer >= 472) // MK9; real version: MidwayVer >= 36
			{
				FStaticMeshNormalStream_MK NormalStream;
				Ar << Lod.VertexStream << Lod.ColorStream << NormalStream << Lod.UVStream << Lod.f80;
				// copy NormalStream into UVStream
				assert(Lod.UVStream.UV.Num() == NormalStream.Normals.Num());
				for (int i = 0; i < Lod.UVStream.UV.Num(); i++)
				{
					FStaticMeshUVItem3  &UV = Lod.UVStream.UV[i];
					FStaticMeshNormal_MK &N = NormalStream.Normals[i];
					UV.Normal[0] = N.Normal[0];
					UV.Normal[1] = N.Normal[1];
					UV.Normal[2] = N.Normal[2];
				}
				goto duplicate_verts;
			}
#endif // MKVSDC
			Ar << Lod.VertexStream;
			Ar << Lod.UVStream;
			Ar << Lod.f80;
#if MKVSDC || AVA
			if (Ar.Game == GAME_MK || Ar.Game == GAME_AVA)
			{
			duplicate_verts:
				// note: sometimes UVStream has 2 times more items than VertexStream
				// we should duplicate vertices
				int n1 = Lod.VertexStream.Verts.Num();
				int n2 = Lod.UVStream.UV.Num();
				if (n1 * 2 == n2)
				{
					appPrintf("Duplicating MK StaticMesh verts\n");
					Lod.VertexStream.Verts.Add(n1);
					for (int i = 0; i < n1; i++)
						Lod.VertexStream.Verts[i+n1] = Lod.VertexStream.Verts[i];
				}
			}
#endif // MKVSDC || AVA
		}
		else if (Ar.ArVer >= 364)
		{
		// ver_2:
			Ar << Lod.UVStream;
			Ar << Lod.f80;
			// create VertexStream
			int NumVerts = Lod.UVStream.UV.Num();
			Lod.VertexStream.Verts.Empty(NumVerts);
//			Lod.VertexStream.NumVerts = NumVerts;
			for (int i = 0; i < NumVerts; i++)
				Lod.VertexStream.Verts.AddItem(Lod.UVStream.UV[i].Pos);
		}
		else
		{
		// ver_1:
			TArray<FStaticMeshUVStream3Old> UVStream;
			if (Ar.ArVer >= 333)
			{
				appNotify("StaticMesh: untested code! (ArVer=%d)", Ar.ArVer);
				TArray<FQuat> Verts;
				TArray<int>   Normals;	// compressed
				Ar << Verts << Normals << UVStream;	// really used RAW_ARRAY, but it is too new for this code
				//!! convert
			}
			else
			{
				// oldest version
				TArray<FStaticMeshVertex3Old> Verts;
				Ar << Verts << UVStream;
				// convert vertex stream
				int i;
				int NumVerts     = Verts.Num();
				int NumTexCoords = UVStream.Num();
				if (NumTexCoords > NUM_MESH_UV_SETS)
				{
					appNotify("StaticMesh has %d UV sets", NumTexCoords);
					NumTexCoords = NUM_MESH_UV_SETS;
				}
				Lod.VertexStream.Verts.Empty(NumVerts);
				Lod.VertexStream.Verts.Add(NumVerts);
				Lod.UVStream.UV.Empty();
				Lod.UVStream.UV.Add(NumVerts);
				Lod.UVStream.NumVerts     = NumVerts;
				Lod.UVStream.NumTexCoords = NumTexCoords;
				// resize UV streams
				for (i = 0; i < NumVerts; i++)
				{
					FStaticMeshVertex3Old &V = Verts[i];
					FVector              &DV = Lod.VertexStream.Verts[i];
					FStaticMeshUVItem3   &UV = Lod.UVStream.UV[i];
					DV           = V.Pos;
					UV.Normal[2] = V.Normal[2];
					for (int j = 0; j < NumTexCoords; j++)
						UV.UV[j] = UVStream[j].Data[i];
				}
			}
		}
	indices:
		Ar << Lod.Indices;
#if ENDWAR
		if (Ar.Game == GAME_EndWar) goto after_indices;	// single Indices buffer since version 262
#endif
#if APB
		if (Ar.Game == GAME_APB)
		{
			// serialized FIndexBuffer3 guarded by APB bulk seeker (check UTexture3::Serialize() for details)
			Ar.Seek(Ar.Tell() + 8);
			goto after_indices;				// do not need this data
		}
#endif // APB
		Ar << Lod.Indices2;
	after_indices:

		if (Ar.ArVer < 686)
		{
			Ar << RAW_ARRAY(Lod.Edges);
			Ar << Lod.fEC;
		}
#if ALPHA_PR
		if (Ar.Game == GAME_AlphaProtocol)
		{
			assert(Ar.ArLicenseeVer > 8);	// ArLicenseeVer = [1..7] has custom code
			if (Ar.ArLicenseeVer >= 4)
			{
				TArray<int> unk128;
				Ar << RAW_ARRAY(unk128);
			}
		}
#endif // ALPHA_PR
#if AVA
		if (Ar.Game == GAME_AVA)
		{
			if (Ar.ArLicenseeVer >= 2)
			{
				int fFC, f100;
				Ar << fFC << f100;
			}
			if (Ar.ArLicenseeVer >= 4)
			{
				FByteBulkData f104, f134, f164, f194, f1C4, f1F4, f224, f254;
				f104.Skip(Ar);
				f134.Skip(Ar);
				f164.Skip(Ar);
				f194.Skip(Ar);
				f1C4.Skip(Ar);
				f1F4.Skip(Ar);
				f224.Skip(Ar);
				f254.Skip(Ar);
			}
		}
#endif // AVA

		if (Ar.ArVer >= 841)
		{
			FIndexBuffer3 Indices3;
			Ar << Indices3;
			if (Indices3.Indices.Num())
				appPrintf("LOD has extra index buffer (%d items)\n", Indices3.Indices.Num());
		}

		return Ar;

		unguard;
	}
};

struct FkDOPBounds		// bounds for compressed (quantized) kDOP node
{
	FVector				v1;
	FVector				v2;

	friend FArchive& operator<<(FArchive &Ar, FkDOPBounds &V)
	{
#if ENSLAVED
		if (Ar.Game == GAME_Enslaved)
		{
			// compressed structure
			short v1[3], v2[3];
			Ar << v1[0] << v1[1] << v1[2] << v2[0] << v2[1] << v2[2];
			return Ar;
		}
#endif // ENSLAVED
		return Ar << V.v1 << V.v2;
	}
};

struct FkDOPNode3
{
	FkDOPBounds			Bounds;
	int					f18;
	short				f1C;
	short				f1E;

	friend FArchive& operator<<(FArchive &Ar, FkDOPNode3 &V)
	{
#if ENSLAVED
		if (Ar.Game == GAME_Enslaved)
		{
			// all data compressed
			byte  fC, fD;
			short fE;
			Ar << V.Bounds;		// compressed
			Ar << fC << fD << fE;
			return Ar;
		}
#endif // ENSLAVED
#if DCU_ONLINE
		if (Ar.Game == GAME_DCUniverse && (Ar.ArLicenseeVer & 0xFF00) >= 0xA00)
			return Ar << V.f18 << V.f1C << V.f1E;	// no Bounds field - global for all nodes
#endif // DCU_ONLINE
		Ar << V.Bounds << V.f18;
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 7) goto new_ver;
#endif
#if MOHA
		if (Ar.Game == GAME_MOHA && Ar.ArLicenseeVer >= 8) goto new_ver;
#endif
#if MKVSDC
		if (Ar.Game == GAME_MK) goto old_ver;
#endif
		if ((Ar.ArVer < 209) || (Ar.ArVer >= 468))
		{
		new_ver:
			Ar << V.f1C << V.f1E;	// short
		}
		else
		{
		old_ver:
			// old version
			assert(Ar.IsLoading);
			int tmp1C, tmp1E;
			Ar << tmp1C << tmp1E;
			V.f1C = tmp1C;
			V.f1E = tmp1E;
		}
		return Ar;
	}
};

struct FkDOPNode3New	// starting from version 770
{
	byte				mins[3];
	byte				maxs[3];

	friend FArchive& operator<<(FArchive &Ar, FkDOPNode3New &V)
	{
		Ar << V.mins[0] << V.mins[1] << V.mins[2] << V.maxs[0] << V.maxs[1] << V.maxs[2];
		return Ar;
	}
};

SIMPLE_TYPE(FkDOPNode3New, byte)


struct FkDOPTriangle3
{
	short				f0, f2, f4, f6;

	friend FArchive& operator<<(FArchive &Ar, FkDOPTriangle3 &V)
	{
#if FURY
		if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 25) goto new_ver;
#endif
#if FRONTLINES
		if (Ar.Game == GAME_Frontlines && Ar.ArLicenseeVer >= 7) goto new_ver;
#endif
#if MOHA
		if (Ar.Game == GAME_MOHA && Ar.ArLicenseeVer >= 8) goto new_ver;
#endif
#if MKVSDC
		if (Ar.Game == GAME_MK) goto old_ver;
#endif
		if ((Ar.ArVer < 209) || (Ar.ArVer >= 468))
		{
		new_ver:
			Ar << V.f0 << V.f2 << V.f4 << V.f6;
		}
		else
		{
		old_ver:
			assert(Ar.IsLoading);
			int tmp0, tmp2, tmp4, tmp6;
			Ar << tmp0 << tmp2 << tmp4 << tmp6;
			V.f0 = tmp0;
			V.f2 = tmp2;
			V.f4 = tmp4;
			V.f6 = tmp6;
		}
		return Ar;
	}
};


#if FURY

struct FFuryStaticMeshUnk	// in other ganes this structure serialized after LOD models, in Fury - before
{
	int					unk0;
	int					fC, f10, f14;
	TArray<short>		f18;				// old version uses TArray<int>, new - TArray<short>, but there is no code selection
											// (array size in old version is always 0?)

	friend FArchive& operator<<(FArchive &Ar, FFuryStaticMeshUnk &S)
	{
		if (Ar.ArVer < 297) Ar << S.unk0;	// Version? (like in FIndexBuffer3)
		if (Ar.ArLicenseeVer >= 4)			// Fury-specific
			Ar << S.fC << S.f10 << S.f14 << S.f18;
		return Ar;
	}
};

#endif // FURY


struct FStaticMeshUnk5
{
	int					f0;
	byte				f4[3];

	friend FArchive& operator<<(FArchive &Ar, FStaticMeshUnk5 &S)
	{
		return Ar << S.f0 << S.f4[0] << S.f4[1] << S.f4[2];
	}
};


void UStaticMesh3::Serialize(FArchive &Ar)
{
	guard(UStaticMesh3::Serialize);

	Super::Serialize(Ar);

#if FURY
	if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 14)
	{
		int unk3C, unk40;
		Ar << unk3C << unk40;
	}
#endif // FURY
#if DARKVOID
	if (Ar.Game == GAME_DarkVoid)
	{
		int unk180, unk18C, unk198;
		if (Ar.ArLicenseeVer >= 5) Ar << unk180 << unk18C;
		if (Ar.ArLicenseeVer >= 6) Ar << unk198;
	}
#endif // DARKVOID
#if TERA
	if (Ar.Game == GAME_Tera && Ar.ArLicenseeVer >= 3)
	{
		FString SourceFileName;
		Ar << SourceFileName;
	}
#endif // TERA
#if TRANSFORMERS
	if (Ar.Game == GAME_Transformers && Ar.ArLicenseeVer >= 50)
	{
		Ar << RAW_ARRAY(kDOPNodes) << RAW_ARRAY(kDOPTriangles) << Lods;
		// note: Bounds is serialized as property (see UStaticMesh in h-file)
		goto done;
	}
#endif // TRANSFORMERS

	Ar << Bounds << BodySetup;
#if TUROK
	if (Ar.Game == GAME_Turok && Ar.ArLicenseeVer >= 59)
	{
		int unkFC, unk100;
		Ar << unkFC << unk100;
	}
#endif // TUROK
	if (Ar.ArVer < 315)
	{
		UObject *unk;
		Ar << unk;
	}
#if ENDWAR
	if (Ar.Game == GAME_EndWar) goto version;	// no kDOP since version 306
#endif // ENDWAR
#if SINGULARITY
	if (Ar.Game == GAME_Singularity)
	{
		// serialize kDOP tree
		assert(Ar.ArLicenseeVer >= 112);
		// old serialization code
		Ar << RAW_ARRAY(kDOPNodes) << RAW_ARRAY(kDOPTriangles);
		// new serialization code
		// bug in Singularity serialization code: serialized the same things twice!
		goto ver_770;
	}
#endif // SINGULARITY
#if BULLETSTORM
	if (Ar.Game == GAME_Bulletstorm && Ar.ArVer >= 739) goto ver_770;
#endif
	// kDOP tree
	if (Ar.ArVer < 770)
	{
		Ar << RAW_ARRAY(kDOPNodes);
	}
	else
	{
	ver_770:
		FkDOPBounds Bounds;
		TArray<FkDOPNode3New> Nodes;
		Ar << Bounds << RAW_ARRAY(Nodes);
	}
#if FURY
	if (Ar.Game == GAME_Fury && Ar.ArLicenseeVer >= 32)
	{
		int kDopUnk;
		Ar << kDopUnk;
	}
#endif // FURY
	Ar << RAW_ARRAY(kDOPTriangles);
#if DCU_ONLINE
	if (Ar.Game == GAME_DCUniverse && (Ar.ArLicenseeVer & 0xFF00) >= 0xA00)
	{
		// this game stored kDOP bounds only once
		FkDOPBounds Bounds;
		Ar << Bounds;
	}
#endif // DCU_ONLINE
#if DOH
	if (Ar.Game == GAME_DOH && Ar.ArLicenseeVer >= 73)
	{
		FVector			unk18;		// extra computed kDOP field
		TArray<FVector>	unkA0;
		int				unk74;
		Ar << unk18;
		Ar << InternalVersion;		// has InternalVersion = 0x2000F
		Ar << unkA0 << unk74 << Lods;
		goto done;
	}
#endif // DOH

version:
	Ar << InternalVersion;

#if DEBUG_STATICMESH
	appPrintf("kDOPNodes=%d kDOPTriangles=%d\n", kDOPNodes.Num(), kDOPTriangles.Num());
	appPrintf("ver: %d\n", InternalVersion);
#endif

#if FURY
	if (Ar.Game == GAME_Fury)
	{
		int unk1, unk2;
		TArray<FFuryStaticMeshUnk> unk50;
		if (Ar.ArLicenseeVer >= 34) Ar << unk1;
		if (Ar.ArLicenseeVer >= 33) Ar << unk2;
		if (Ar.ArLicenseeVer >= 8)  Ar << unk50;
		InternalVersion = 16;		// uses InternalVersion=18
	}
#endif // FURY
#if TRANSFORMERS
	if (Ar.Game == GAME_Transformers) goto lods;	// The Bourne Conspiracy has InternalVersion=17
#endif

	if (InternalVersion >= 17 && Ar.ArVer < 593)
	{
		TArray<FName> unk;			// some text properties; ContentTags ? (switched from binary to properties)
		Ar << unk;
	}
	if (Ar.ArVer >= 823)
	{
		guard(SerializeExtraLOD);

		int unkFlag;
		FStaticMeshLODModel unkLod;
		Ar << unkFlag;
		if (unkFlag)
		{
			appPrintf("has extra LOD model\n");
			Ar << unkLod;
		}

		if (Ar.ArVer < 829)
		{
			TArray<int> unk;
			Ar << unk;
		}
		else
		{
			TArray<FStaticMeshUnk5> f178;
			Ar << f178;
		}
		int f74;
		Ar << f74;

		unguard;
	}
#if SHADOWS_DAMNED
	if (Ar.Game == GAME_ShadowsDamned && Ar.ArLicenseeVer >= 26)
	{
		int unk134;
		Ar << unk134;
	}
#endif // SHADOWS_DAMNED

lods:
	Ar << Lods;

//	Ar << f48;

done:
	DROP_REMAINING_DATA(Ar);

	// convert UStaticMesh3 to CStaticMesh

	guard(ConvertMesh);

	CStaticMesh *Mesh = new CStaticMesh(this);
	ConvertedMesh = Mesh;

	// convert bounds
	Mesh->BoundingSphere.R = Bounds.SphereRadius / 2;		//?? UE3 meshes has radius 2 times larger than mesh
	VectorSubtract(CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Min));
	VectorAdd     (CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Max));

	// convert lods
	Mesh->Lods.Empty(Lods.Num());
	for (int lod = 0; lod < Lods.Num(); lod++)
	{
		guard(ConvertLod);

		const FStaticMeshLODModel &SrcLod = Lods[lod];
		CStaticMeshLod *Lod = new (Mesh->Lods) CStaticMeshLod;

		int NumTexCoords = SrcLod.UVStream.NumTexCoords;
		int NumVerts     = SrcLod.VertexStream.Verts.Num();

		Lod->NumTexCoords = NumTexCoords;
		Lod->HasTangents  = (Ar.ArVer >= 364);				//?? check; FStaticMeshUVStream3 is used since this version
		if (NumTexCoords > NUM_MESH_UV_SETS)
			appError("StaticMesh has %d UV sets", NumTexCoords);

		// sections
		Lod->Sections.Add(SrcLod.Sections.Num());
		for (int i = 0; i < SrcLod.Sections.Num(); i++)
		{
			CStaticMeshSection &Dst = Lod->Sections[i];
			const FStaticMeshSection3 &Src = SrcLod.Sections[i];
			Dst.Material   = Src.Mat;
			Dst.FirstIndex = Src.FirstIndex;
			Dst.NumFaces   = Src.NumFaces;
		}

		// vertices
		Lod->AllocateVerts(NumVerts);
		for (int i = 0; i < NumVerts; i++)
		{
			const FStaticMeshUVItem3 &SUV = SrcLod.UVStream.UV[i];
			CStaticMeshVertex &V = Lod->Verts[i];

			V.Position = CVT(SrcLod.VertexStream.Verts[i]);
			UnpackNormals(SUV.Normal, V);
			// copy UV
			staticAssert((sizeof(CMeshUVFloat) == sizeof(FMeshUVFloat)) && (sizeof(V.UV) == sizeof(SUV.UV)), Incompatible_CStaticMeshUV);
#if 0
			for (int j = 0; j < NumTexCoords; j++)
				V.UV[j] = (CMeshUVFloat&/SUV.UV[j];
#else
			memcpy(V.UV, SUV.UV, sizeof(V.UV));
#endif
			//!! also has ColorStream
		}

		// indices
		CopyArray(Lod->Indices.Indices16, SrcLod.Indices.Indices);	//!! 16-bit only; place to CStaticMesh cpp

		unguardf(("lod=%d", lod));
	}

	unguard;	// ConvertMesh

	unguard;
}


#endif // UNREAL3