#include "UERLBatchStaging.h"

namespace
{
	bool ValidateBindings(const TArray<FUERLBatchFieldBinding>& Bindings, int32 Width)
	{
		if (Width <= 0)
		{
			return false;
		}

		TBitArray<> Claimed(false, Width);
		TSet<FName> Names;
		for (const FUERLBatchFieldBinding& Binding : Bindings)
		{
			if (!Binding.Field.IsValid() || Binding.Column < 0 || Binding.Column > Width
				|| Binding.Field.Width > Width - Binding.Column
				|| Names.Contains(Binding.Field.Name))
			{
				return false;
			}
			Names.Add(Binding.Field.Name);
			const int32 EndColumn = Binding.Column + Binding.Field.Width;
			for (int32 Column = Binding.Column; Column < EndColumn; ++Column)
			{
				if (Claimed[Column])
				{
					return false;
				}
				Claimed[Column] = true;
			}
		}
		return Claimed.Find(false) == INDEX_NONE;
	}
}

bool FUERLBatchSchema::IsValid() const
{
	return ResetWidth >= 0
		&& ValidateBindings(ActionFields, ActionWidth)
		&& ValidateBindings(StateFields, StateWidth);
}
