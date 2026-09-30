#include "UERLProvider.h"

const FUERLResetRow* FUERLResetBatch::Find(int32 SlotId) const
{
	return Rows.FindByPredicate([SlotId](const FUERLResetRow& Row) { return Row.SlotId == SlotId; });
}
