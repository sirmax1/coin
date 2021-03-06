/*######     Copyright (c) 1997-2013 Ufasoft  http://ufasoft.com  mailto:support@ufasoft.com,  Sergey Pavlov  mailto:dev@ufasoft.com #######################################
#                                                                                                                                                                          #
# This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation;  #
# either version 3, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the      #
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU #
# General Public License along with this program; If not, see <http://www.gnu.org/licenses/>                                                                               #
##########################################################################################################################################################################*/

#include <el/ext.h>

#include "dblite.h"
#include "b-tree.h"

namespace Ext { namespace DB { namespace KV {


int NumKeys(const Page& page) {
	return page.Header().Num;
}



size_t CalculateLocalDataSize(UInt64 dataSize, size_t keyFullSize, size_t pageSize) {
	if (dataSize > pageSize || dataSize+keyFullSize>(pageSize-3)) {
		size_t rem = dataSize % (pageSize-4);
		return rem+keyFullSize+4 > (pageSize-3) ? 0 : rem;
	}
	return (size_t)dataSize;
}

struct LiteEntry {
	byte *P;

	ConstBuf Key(byte keySize) {
		return keySize ? ConstBuf(P, keySize) : ConstBuf(P+1, P[0]);
	}

	UInt32 PgNo() {
		return GetLeUInt32(P-4);
	}

	void PutPgNo(UInt32 v) {
		PutLeUInt32(P-4, v);
	}

	UInt64 DataSize(byte keySize) {
		const byte *p = P + (keySize ? keySize : 1+P[0]);
		return Read7BitEncoded(p);
	}

	ConstBuf LocalData(size_t pageSize, byte keySize) {
		const byte *p = P + (keySize ? keySize : 1+P[0]);
		UInt64 dataSize = Read7BitEncoded(p);
		return ConstBuf(p, CalculateLocalDataSize(dataSize, p-P, pageSize));
	}

	UInt32 FirstBigdataPage() {
		return (this+1)->PgNo();
	}

	byte *Upper() { return (this+1)->P; }

	size_t Size() { return Upper()-P; }
};

LiteEntry *BuildBranchLiteEntryIndex(PageHeader& h, void *raw, int n, size_t pageSize, byte keySize) {
	LiteEntry *entries = (LiteEntry*)raw;
	byte *p = h.Data+4;
	for (int i=0; i<n; ++i) {
		entries[i].P = p;
		p += (keySize ? keySize : 1 + p[0]) + 4;
	}
	entries[n].P = p;
	ASSERT(p - (byte*)&h <= pageSize);
	return entries;
}

LiteEntry *BuildLiteEntryIndex(PageHeader& h, void *raw, int n, size_t pageSize, byte keySize) {
	if (h.Flags & PAGE_FLAG_BRANCH)
		return BuildBranchLiteEntryIndex(h, raw, n, pageSize, keySize);
	LiteEntry *entries = (LiteEntry*)raw;
	byte *p = h.Data, *q=p;
	for (int i=0; i<n; ++i, q=p) {
		entries[i].P = p;
		p += keySize ? keySize : 1 + p[0];
		UInt64 dataSize = Read7BitEncoded((const byte*&)p);
		size_t cbLocal = CalculateLocalDataSize(dataSize, p-q, pageSize);
		p += cbLocal + (cbLocal!=dataSize ? 4 : 0);
	}
	entries[n].P = p;
	ASSERT(p - (byte*)&h <= pageSize);
	return entries;
}

LiteEntry *Page::Entries(byte keySize) const {
#ifdef X_DEBUG//!!!D
	if (m_pimpl->Entries) {
		PageHeader& hh = Header();
		LiteEntry *le;
		BuildLiteEntryIndex(hh, le = (LiteEntry*)alloca(sizeof(LiteEntry) * (hh.Num+1)), hh.Num);
		if (memcmp(le, m_pimpl->Entries, sizeof(LiteEntry) * (hh.Num+1)))
			cout << "Error";
	}
#endif

	if (!m_pimpl->Entries) {
		PageHeader& h = Header();
		LiteEntry *p = BuildLiteEntryIndex(h, Malloc(sizeof(LiteEntry) * (h.Num+1)), h.Num, m_pimpl->Storage.PageSize, keySize);
		if (Interlocked::CompareExchange(m_pimpl->Entries, p, (LiteEntry*)0))
			Ext::Free(p);		
	}
	return m_pimpl->Entries;
}

#ifdef X_DEBUG//!!!D

void CheckPage(Page& page) {
	PageHeader& h = page.Header();
	LiteEntry *entries = BuildLiteEntryIndex(h, (LiteEntry*)alloca(sizeof(LiteEntry) * (h.Num+1)), h.Num);
	for (int i=0; i<h.Num; ++i) {
		ASSERT(entries[i].PgNo() < 100000);
	}
}

#endif

size_t Page::SizeLeft(byte keySize) const {
	PageHeader& h = Header();
	return m_pimpl->Storage.PageSize - (Entries(keySize)[h.Num].P - (byte*)&h);
}

EntryDesc GetEntryDesc(const PagePos& pp, byte keySize) {
	PageHeader& h = pp.Page.Header();
	ASSERT(pp.Pos < h.Num);
	LiteEntry *entries = pp.Page.Entries(keySize),
		&e = entries[pp.Pos];

	EntryDesc r;
	ZeroStruct(r);
	r.P = e.P;
	r.Size = e.Size();
	size_t pageSize = pp.Page.m_pimpl->Storage.PageSize;
	if (pp.Page.IsBranch)
		r.PgNo = e.PgNo();
	else {
		r.DataSize = e.DataSize(keySize);
		r.LocalData = e.LocalData(pageSize, keySize);
		if (r.Overflowed = r.DataSize >= pageSize || r.LocalData.P+r.DataSize!=e.Upper()) {
			r.PgNo = e.FirstBigdataPage();
		}
	}
	return r;
}

LiteEntry GetLiteEntry(const PagePos& pp, byte keySize) {
	PageHeader& h = pp.Page.Header();
	ASSERT(pp.Pos <= h.Num);

	return pp.Page.Entries(keySize)[pp.Pos];
//!!!	return BuildLiteEntryIndex(h, alloca(sizeof(LiteEntry) * (pp.Pos+2)), pp.Pos+1)[pp.Pos];
}

pair<size_t, bool> GetDataEntrySize(size_t ksize, UInt64 dsize, size_t pageSize) {
	byte buf[10], *p = buf;
	Write7BitEncoded(p, dsize);
	return make_pair(CalculateLocalDataSize(dsize, ksize + (p-buf), pageSize), ksize + (p-buf) + dsize > pageSize-3);
}

int Page::FillPercent(byte keySize) {
	LiteEntry *entries = Entries(keySize);
	PageHeader& h = Header();
	int pageSize = m_pimpl->Storage.PageSize;
	return h.Num ? (pageSize - (entries[h.Num].P - h.Data))*100/pageSize : 0;
}

bool Page::IsUnderflowed(byte keySize) {
	return FillPercent(keySize) < FILL_THRESHOLD_PERCENTS;
}

void InsertCell(const PagePos& pagePos, const ConstBuf& cell, byte keySize) {
	Page page = pagePos.Page;
	PageObj& po = *page.m_pimpl;
	if (po.Overflows || page.SizeLeft(keySize) < cell.Size) {
		ASSERT(po.Overflows < _countof(po.OverflowCells));
		po.OverflowCells[po.Overflows++] = IndexedBuf(cell.P, (UInt16)cell.Size, (UInt16)pagePos.Pos);
	} else {
		PageHeader& h = page.Header();
		bool bBranch = h.Flags & PAGE_FLAG_BRANCH;
		int off = bBranch ? 4 : 0;
		LiteEntry *entries = page.Entries(keySize),
			&e = entries[pagePos.Pos];
		memmove(e.P+cell.Size-off, e.P-off, entries[h.Num++].P-e.P+off);
		memcpy(e.P-off, cell.P, cell.Size);
	}
	page.ClearEntries();
}

// Keeps Entry Index valid on the same memory place
// Returns first overflow page or 0
UInt32 DeleteEntry(const PagePos& pp, byte keySize) {
	PageHeader& h = pp.Page.Header();
	ASSERT(pp.Pos < h.Num);
	LiteEntry *entries = pp.Page.Entries(keySize),
		&e = entries[pp.Pos];
	bool bBranch = pp.Page.IsBranch;
	int r = bBranch || e.DataSize(keySize) == e.LocalData(pp.Page.m_pimpl->Storage.PageSize, keySize).Size ? 0 : e.FirstBigdataPage();
	ssize_t off = e.Size();
	byte *p = e.P - (int(bBranch)*4);
	memmove(p, p+off, entries[h.Num--].P - p - off);
	for (int i=pp.Pos+1; i<=h.Num; ++i)
		entries[i].P = entries[i+1].P-off;
	return r;
}

// Returns pointer to the RightPtr of the Branch Page
static byte *AssemblePage(Page& page, const ConstBuf cells[], UInt16 n) {
	ASSERT(!page.Overflows);

	PageHeader& h = page.Header();
	h.Num = htole(n);
	byte *p = h.Data;
	for (int i=0; i<n; ++i) {
		const ConstBuf& cell = cells[i];
		memcpy(exchange(p, p+cell.Size), cell.P, cell.Size);
	}
	return p;
}

void BTree::SetRoot(const Page& page) {
	Root = page;
	Dirty = true;
	if (Name == DbTable::Main.Name)
		Tx.MainTableRoot = page;
}

// second arg not used, but present to ensure it exists in memory when reopened by OpenPage()
void BTree::BalanceNonRoot(PagePos& ppParent, Page&, byte *tmpPage) {
	KVStorage& stg = Tx.Storage;
	const size_t pageSize = stg.PageSize;
	Page pageParent = ppParent.Page;

	int n = NumKeys(pageParent);
	int	nxDiv = std::max(0, std::min(ppParent.Pos-1, n-2+int(Tx.Bulk)));
	n = std::min(2-int(Tx.Bulk), n);
	vector<Page> oldPages(n+1);
	vector<ConstBuf> dividers(n);
	LiteEntry *entriesParent = pageParent.Entries(KeySize);
	byte *tmp = tmpPage;
	int nEstimatedCells = 0;
	for (int i=oldPages.size()-1; i>=0; --i) {
		LiteEntry& e = entriesParent[nxDiv+i];
		Page& page = oldPages[i] = Tx.OpenPage(e.PgNo());
		nEstimatedCells += NumKeys(page)+page.Overflows+1;
		if (i != oldPages.size()-1) {
			size_t cbEntry = e.Size();
			memcpy(tmp, e.P-4, cbEntry);
			dividers[i] = ConstBuf(tmp, cbEntry);
			tmp += cbEntry;
			DeleteEntry(PagePos(pageParent, nxDiv+i), KeySize);
		}
	}
	bool bBranch = oldPages.front().IsBranch;

	struct PageInfo {
		size_t UsedSize;
		int Count;
		Ext::DB::KV::Page Page;

		PageInfo(size_t usedSize = 0, int count = 0)
			:	UsedSize(usedSize)
			,	Count(count)		
		{}
	};
	AlignedMem memTmp((oldPages.size()+1)*pageSize, 64);
	vector<ConstBuf> cells;
	cells.reserve(nEstimatedCells);
	byte *pTemp = (byte*)memTmp.get()+oldPages.size()*pageSize;
	UInt32 pgnoRight = 0;
	for (int i=0; i<oldPages.size(); ++i) {
		Page& page = oldPages[i];
		PageHeader& h = page.Header();
		LiteEntry *entries = page.Entries(KeySize);
		PageObj& po = *page.m_pimpl;
		ASSERT(po.Dirty || !po.Overflows);

		if (po.Dirty) {
			PageHeader& hd = *(PageHeader*)((byte*)memTmp.get()+i*stg.PageSize);
			MemcpyAligned32(&hd, &h, entries[h.Num].P - (byte*)&h);
			ssize_t off = (byte*)&hd - (byte*)&h;
			for (int i=0, n=h.Num; i<=n; ++i)
				entries[i].P += off;
		}
		for (int k=0, n=NumKeys(page) + po.Overflows, idx; (idx=k)<n; ++k) {
			ConstBuf cell;
			for (int j=int(po.Overflows)-1; j>=0; --j) {
				IndexedBuf& ib = po.OverflowCells[j];
				if (idx == ib.Index) {
					cell = ConstBuf(ib.P, ib.Size);
					goto LAB_FOUND;
				} else if (idx > ib.Index)
					--idx;
			}
			{
				LiteEntry& e = entries[idx];
				cell = ConstBuf(e.P-int(bBranch)*4, e.Size());
			}
LAB_FOUND:
			cells.push_back(cell);
		}
		po.Overflows = 0;
		if (bBranch) {
			UInt32 pgno = entries[h.Num].PgNo();
			if (i == oldPages.size()-1)
				pgnoRight = pgno;
			else {
				PutLeUInt32(pTemp, pgno);
				ConstBuf& div = dividers[i];
				memcpy(pTemp+4, div.P+4, div.Size-4);
				cells.push_back(ConstBuf(pTemp, div.Size));
				pTemp += div.Size;
			}
		}
		if (po.Dirty)
			page.ClearEntries();
	}
	ASSERT(!cells.empty());
	
	vector<PageInfo> newPages;
	const size_t cbUsable = stg.PageSize-3-int(bBranch)*4;
	size_t cbPage = 0;
	int iCell = 0;
	for (; iCell<cells.size(); ++iCell) {
		size_t cb = cells[iCell].Size;
		if (cbPage+cb > cbUsable) {
			newPages.push_back(PageInfo(exchange(cbPage, 0), iCell));
			iCell -= (int)(!bBranch);
		} else
			cbPage += cb;
	}
	newPages.push_back(PageInfo(exchange(cbPage, 0), iCell));

	for (int i=newPages.size()-1; i>0; --i) {
		size_t &cbRight = newPages[i].UsedSize,
			&cbLeft = newPages[i-1].UsedSize;
		int& cnt = newPages[i-1].Count;
		while (true) {
			int r = cnt - 1,
				d = r + 1 - (bBranch ? 0 : 1);
			if (cbRight!=0 && (Tx.Bulk || cbRight+cells[d].Size > cbLeft-cells[r].Size))
				break;
			cbRight += cells[d].Size;
			cbLeft -= cells[r].Size;
			--cnt;
		}
	}

	for (int i=0; i<newPages.size(); ++i) {
		for (vector<Page>::iterator it=oldPages.begin(); it!=oldPages.end(); ++it) {
			if (it->Dirty) {
				ASSERT(!it->m_pimpl->Entries);

				newPages[i].Page = *it;
				oldPages.erase(it);
				break;
			}
		}
		if (!newPages[i].Page)
			newPages[i].Page = Tx.Allocate(bBranch);
	}

	//!!!TODO Sort pgnos

	entriesParent[nxDiv].PutPgNo(newPages.back().Page.N);
	
	int j = 0;
	tmp = tmpPage;
	for (int i=0; i<newPages.size(); ++i) {
		Page& page = newPages[i].Page;
		ASSERT(!page.Overflows);

		int nj = newPages[i].Count;
		byte *pRight = AssemblePage(page, &cells[j], UInt16(nj-j));
		j = nj;

		ASSERT(i<newPages.size()-1 || j==cells.size());
		if (j < cells.size()) {
			ConstBuf div = cells[j];
			if (bBranch) {
				memcpy(pRight, div.P, 4);
				++j;
			} else {
				div.Size = 4 + (KeySize ? KeySize : 1+div.P[0]);
				div.P -= 4;
			}
			PutLeUInt32(tmp, page.N);
			memcpy(tmp+4, div.P+4, div.Size-4);
			InsertCell(PagePos(pageParent, nxDiv++), ConstBuf(tmp, div.Size), KeySize);
			tmp += div.Size;
		}
		if (bBranch && i==newPages.size()-1)
			PutLeUInt32(pRight, pgnoRight);
	}

	for (int i=0; i<oldPages.size(); ++i)						// non-Dirty oldPages contain used Data, so free them only after copying to newPages
		Tx.FreePage(oldPages[i]);

	if (pageParent==Root && 0==NumKeys(pageParent)) {
		ASSERT(newPages.size() == 1);
		Tx.FreePage(Root);
		SetRoot(newPages[0].Page);
	}
}

void DbCursor::Balance() {
	if (Path.empty())
		return;
	Page& page = Top().Page;
	if (Path.size() == 1) {
		if (!page.Overflows)
			return;
		Tree->SetRoot(Tree->Tx.Allocate(true));
		PutLeUInt32(Tree->Root.Header().Data, page.N);
		Path.insert(Path.begin(), PagePos(Tree->Root, 0));
	} else if (!page.Overflows && !page.IsUnderflowed(Tree->KeySize))
		return;
	Initialized = false;
	Blob blobDivs(0, Tree->Tx.Storage.PageSize);										// should be in dynamic scope during one nested call of Balance()
	Tree->BalanceNonRoot(Path[Path.size()-2], Path.back().Page, blobDivs.data());
	Path.back().Page.m_pimpl->Overflows = 0;
	Path.pop_back();
	Balance();
}

void DbCursor::FreeBigdataPages(UInt32 pgno) {
	KVStorage& stg = Tree->Tx.Storage;
	while (pgno)
		Tree->Tx.FreePage(exchange(pgno, *(BeUInt32*)(Tree->Tx.OpenPage(pgno).get_Address())));
}

void DbCursor::Delete() {
	if (Tree->Tx.ReadOnly)
		Throw(E_ACCESSDENIED);
	if (!Initialized)
		Throw(E_INVALIDARG);
	ASSERT(!Top().Page.IsBranch);

	Touch();	
	FreeBigdataPages(DeleteEntry(Top(), Tree->KeySize));
	Tree->Tx.m_bError = true;
	Balance();
	Tree->Tx.m_bError = false;
}

BTree::BTree(const BTree& v)
	:	Tx(v.Tx)
	,	Name(v.Name)
	,	Root(v.Root)
	,	Dirty(v.Dirty)
	,	KeySize(v.KeySize)
	,	m_pfnCompare(v.m_pfnCompare)
{
}

int BTree::Compare(const void *p1, const void *p2, size_t cb2) {
	size_t cb1 = ((const byte*)p1)[-1];
	int r = memcmp(p1, p2, std::min(cb1, cb2));
	return r ? r
		     : (cb1 < cb2 ? -1 : cb1==cb2 ? 0 : 1);
}

#pragma intrinsic(memcmp)

pair<int, bool> BTree::EntrySearch(LiteEntry *entries, int nKey, bool bIsBranch, const ConstBuf& key) {
	int rc = 0;
	int b = 0;
	for (int count=nKey, m, half; count;) {
		ConstBuf nk = entries[m = b+(half=count/2)].Key(KeySize);
		if (!(rc = m_pfnCompare(nk.P, key.P, key.Size)))
			return make_pair(m, true);
		else if (rc < 0) {
			b = m + 1;
			count -= half + 1;
		} else
			count = half;
	}	
	return make_pair(b, false);
}

const ConstBuf& DbCursor::get_Key() {
	if (!m_key.P)
		m_key = Top().Page.Entries(Tree->KeySize)[Top().Pos].Key(Tree->KeySize);
	return m_key;
}

const ConstBuf& DbCursor::get_Data() {
	if (!m_data.P) {
		EntryDesc e = GetEntryDesc(Top(), Tree->KeySize);
		if (!e.Overflowed)
			m_data = e.LocalData;
		else {
			if (e.DataSize > numeric_limits<size_t>::max())
				Throw(E_FAIL);
			m_bigData.Size = (size_t)e.DataSize;
			size_t off = e.LocalData.Size;
			memcpy(m_bigData.data(), e.LocalData.P, off);
			KVStorage& stg = Tree->Tx.Storage;
			for (UInt32 pgno = e.PgNo; pgno;) {
				Page page = Tree->Tx.OpenPage(pgno);
				size_t size = std::min(stg.PageSize - 4, m_bigData.Size-off);
				memcpy(m_bigData.data()+off, (const byte*)page.get_Address()+4, size);
				off += size;
				pgno = *(BeUInt32*)(page.get_Address());
				ASSERT(pgno < 0x10000000); //!!!
			}
			m_data = m_bigData;
		}			
	}
	return m_data;
}

bool DbCursor::PageSearchRoot(const ConstBuf& k, bool bModify) {
	KVStorage& stg = Tree->Tx.Storage;

	Page page = Path.back().Page;
	while (page.IsBranch) {
		PageHeader& h = page.Header();
		LiteEntry *entries = page.Entries(Tree->KeySize);

		int i = 0;
		if (k.Size > MAX_KEY_SIZE)
			i = h.Num;
		else if (k.P) {
			pair<int, bool> pp = Tree->EntrySearch(entries, h.Num, true, k);
			i = pp.second ? pp.first+1 : pp.first;
		}
		page = Tree->Tx.OpenPage(entries[i].PgNo());
		Path.back().Pos = i;
		Path.push_back(PagePos(page, 0));
		ASSERT(Path.size() < 10);
		if (bModify)
			PageTouch(Path.size()-1);
	}
	ASSERT(!page.IsBranch);
	return true;
}

bool DbCursor::ReturnFromSeekKey(int pos) {
	Top().Pos = pos;
	Eof = false;
	ClearKeyData();
	return Initialized = true;	
}

bool DbCursor::SeekToKey(const ConstBuf& k) {
	if (Tree->KeySize && Tree->KeySize!=k.Size)
		Throw(E_INVALIDARG);
	ASSERT(k.Size>0 && k.Size<128);

	if (Initialized) {
		Page& page = Top().Page;
		PageHeader& h = page.Header();
		if (0 == h.Num) {
			Top().Pos = 0;
			return false;
		}
		LiteEntry *entries = page.Entries(Tree->KeySize);
		ConstBuf nk = entries[0].Key(Tree->KeySize);
		int rc = Tree->m_pfnCompare(nk.P, k.P, k.Size);
		if (0 == rc)
			return ReturnFromSeekKey(0);
		else if (rc < 0) {
			if (h.Num > 1) {
				nk = entries[h.Num-1].Key(Tree->KeySize);
				if (0 == (rc = Tree->m_pfnCompare(nk.P, k.P, k.Size)))
					return ReturnFromSeekKey(h.Num-1);
				else if (rc > 0) {
					if (Top().Pos < h.Num) {
						nk = entries[Top().Pos].Key(Tree->KeySize);
						if (0 == (rc = Tree->m_pfnCompare(nk.P, k.P, k.Size)))
							return ReturnFromSeekKey(Top().Pos);
					}
					goto LAB_ENTRY_SEARCH;
				}
			}
			for (int i=0; i<Path.size()-1; ++i)
				if (Path[i].Pos < NumKeys(Path[i].Page)-1)
					goto LAB_SOME_PARENT_HAVE_RIGHT_SIB;
			Top().Pos = h.Num;
			return false;
		}
LAB_SOME_PARENT_HAVE_RIGHT_SIB:
		if (Path.size() < 2) {
			Top().Pos = 0;
			return false;
		}
	}

	if (!PageSearch(k))
		return false;
LAB_ENTRY_SEARCH:
	Page& page = Top().Page;
	ASSERT(!page.IsBranch);

	PageHeader& h = page.Header();

	LiteEntry *entries = page.Entries(Tree->KeySize);
	pair<int, bool> pp = Tree->EntrySearch(entries, h.Num, false, k);
	Top().Pos = pp.first;
	if (!pp.second)
		return false;
	Eof = false;
	ClearKeyData();
#ifdef _DEBUG//!!!D
	ASSERT(Top().Pos < NumKeys(Top().Page));
#endif
	return Initialized = true;
}

bool DbCursor::SeekToSibling(bool bToRight) {
	if (Path.size() <= 1)
		return false;
	Path.pop_back();
	PagePos& pp = Top();
	if (bToRight ? pp.Pos+1<=NumKeys(pp.Page) : pp.Pos>0)
		pp.Pos += bToRight ? 1 : -1;
	else if (!SeekToSibling(bToRight))
		return false;	
	ASSERT(Top().Page.IsBranch);	
	Path.push_back(PagePos(Tree->Tx.OpenPage(Top().Page.Entries(Tree->KeySize)[Top().Pos].PgNo()), 0));
	return true;
}

void DbCursor::InsertImpHeadTail(pair<size_t, bool>& ppEntry, ConstBuf k, const ConstBuf& head, UInt64 fullSize, UInt32 pgnoTail, size_t pageSize) {
	PagePos& pp = Top();
	LiteEntry *entries = pp.Page.Entries(Tree->KeySize);
	Tree->Tx.TmpPageSpace.Size = pageSize;
	byte *p = Tree->Tx.TmpPageSpace.data(), *q = p;
	if (!Tree->KeySize)
		*p++ = (byte)k.Size;
	memcpy(p, k.P, k.Size);
	Write7BitEncoded(p += k.Size, fullSize);
	memcpy(exchange(p, p+ppEntry.first), head.P, ppEntry.first);
	if (ppEntry.second) {
		size_t cbOverflow = head.Size - ppEntry.first;
		if (0 == cbOverflow)
			PutLeUInt32(exchange(p, p+4), pgnoTail);
		else {
			int n = int((cbOverflow+pageSize-4-1) / (pageSize-4));
			vector<UInt32> pages = Tree->Tx.AllocatePages(n);
			const byte *q = head.P + ppEntry.first;
			size_t off = ppEntry.first;
			for (size_t cbCopy, i=0; cbOverflow; q+=cbCopy, cbOverflow-=cbCopy, ++i) {
				cbCopy = std::min(cbOverflow, pageSize-4);
				Page page = Tree->Tx.OpenPage(pages[i]);
				*(BeUInt32*)page.get_Address() = i==pages.size()-1 ? pgnoTail : pages[i+1];
				memcpy((byte*)page.get_Address()+4, q, cbCopy);
			}
			PutLeUInt32(exchange(p, p+4), pages[0]);
		}			
	}
	Tree->Tx.m_bError = true;
	InsertCell(pp, ConstBuf(q, p-q), Tree->KeySize);
	if (pp.Page.Overflows)
		Balance();
	Tree->Tx.m_bError = false;
}

void DbCursor::InsertImp(ConstBuf k, const ConstBuf& d) {
	size_t pageSize = Tree->Tx.Storage.PageSize;
	pair<size_t, bool> ppEntry = GetDataEntrySize(Tree->KeySize ? Tree->KeySize : 1+k.Size, d.Size, pageSize);
	InsertImpHeadTail(ppEntry, k, d, d.Size, 0, pageSize);
}

void DbCursor::Put(ConstBuf k, const ConstBuf& d, bool bInsert) {
	if (Tree->Tx.ReadOnly)
		Throw(E_ACCESSDENIED);
	ASSERT(k.Size < 128 && (!Tree->KeySize || Tree->KeySize==k.Size));
	bool bExists = false;
	if (!Tree->Root) {
		Page pageRoot = Tree->Tx.Allocate(false);
		Tree->SetRoot(pageRoot);
		Path.push_back(PagePos(pageRoot, 0));
		Initialized = true;
	} else {
		bExists = SeekToKey(k);
		if (bExists && bInsert)
			throw DbException(E_EXT_DB_DupKey, nullptr);
		ASSERT(!bExists || Top().Pos < NumKeys(Top().Page));
		Touch();
		if (bExists) {
			PagePos& pp = Top();
			EntryDesc e = GetEntryDesc(pp, Tree->KeySize);
			if (!e.Overflowed && d.Size == e.DataSize) {
				memcpy((byte*)e.LocalData.P, d.P, d.Size);
				return;
			}
			FreeBigdataPages(DeleteEntry(pp, Tree->KeySize));
		}
	}
	InsertImp(k, d);
}

void DbCursor::PushFront(ConstBuf k, const ConstBuf& d) {
	if (Tree->Tx.ReadOnly)
		Throw(E_ACCESSDENIED);
	ASSERT(k.Size < 128 && (!Tree->KeySize || Tree->KeySize==k.Size));
	bool bExists = false;
	if (!Tree->Root) {
		Page pageRoot = Tree->Tx.Allocate(false);
		Tree->SetRoot(pageRoot);
		Path.push_back(PagePos(pageRoot, 0));
		Initialized = true;
	} else {
		bExists = SeekToKey(k);
		ASSERT(!bExists || Top().Pos < NumKeys(Top().Page));
		Touch();
		if (bExists) {
			PagePos& pp = Top();
			EntryDesc e = GetEntryDesc(pp, Tree->KeySize);
			size_t pageSize = Tree->Tx.Storage.PageSize;
			pair<size_t, bool> ppEntry;
			if (!e.Overflowed ||
				e.LocalData.Size != e.DataSize % (pageSize-4) ||
				(ppEntry = GetDataEntrySize(Tree->KeySize ? Tree->KeySize : 1+k.Size, d.Size+e.DataSize, pageSize)).first == 0)
			{
				Blob newdata = d + get_Data();
				FreeBigdataPages(DeleteEntry(pp, Tree->KeySize));
				InsertImp(k, newdata);
				return;
			}
			Blob head = d + e.LocalData;
			ASSERT(ppEntry.second && ppEntry.first==head.Size%(pageSize-4) && ppEntry.first==(d.Size+e.DataSize)%(pageSize-4));
			DeleteEntry(pp, Tree->KeySize);
			InsertImpHeadTail(ppEntry, k, head, d.Size+e.DataSize, e.PgNo, pageSize);
			return;
		}
	}
	InsertImp(k, d);
}

void DbCursor::DeepFreePage(const Page& page) {
	if (page.IsBranch) {
		LiteEntry *entries = page.Entries(Tree->KeySize);
		for (int i=0, n=NumKeys(page); i<=n; ++i)
			DeepFreePage(Tree->Tx.OpenPage(entries[i].PgNo()));
	} else {
		for (int i=0, n=NumKeys(page); i<n; ++i)
			FreeBigdataPages(GetEntryDesc(PagePos(page, i), Tree->KeySize).PgNo);
	}
	Tree->Tx.FreePage(page);
}

void DbCursor::Drop() {
	if (Tree->Root) {
		DeepFreePage(Tree->Root);
		Tree->SetRoot(nullptr);
	}
}

void static CopyPage(Page& pageFrom, Page& pageTo, byte keySize) {
	PageHeader& h = pageFrom.Header();
	LiteEntry *entries = pageFrom.Entries(keySize);
	
	size_t size = entries[h.Num].P - (byte*)&h;
	MemcpyAligned32(pageTo.Address, pageFrom.Address, size);
#ifdef X_DEBUG//!!!D
	memset((byte*)pageTo.Address+size, 0xFE, pageFrom.m_pimpl->Storage.PageSize-size);
#endif

	LiteEntry *dEntries = pageTo.m_pimpl->Entries = (LiteEntry*)Ext::Malloc(sizeof(LiteEntry)*(h.Num+1));
	ssize_t off = (byte*)pageTo.get_Address() - (byte*)pageFrom.get_Address();
	for (int i=0, n=h.Num; i<=n; ++i)
		dEntries[i].P = entries[i].P+off;
}

void DbCursor::PageTouch(int height) {
	Page& page = Path[height].Page;
	DbTransaction& tx = Tree->Tx;
	if (!page.Dirty) {
		Page np = tx.Allocate(page.IsBranch);
		CopyPage(page, np, Tree->KeySize);
		tx.FreePage(page);

		int t = Path.size()-1;
		EXT_FOR (DbCursor& c, Tree->Cursors) {
			if (&c != this && c.Path.size() > t && c.Path[t].Page == page)
				c.Path[t].Page = np;
		}
		page = np;
		if (height)
			GetLiteEntry(Path[height-1], Tree->KeySize).PutPgNo(page.N);
		else
			Tree->SetRoot(page);
	}
}

}}} // Ext::DB::KV::

