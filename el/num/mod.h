/*######     Copyright (c) 1997-2013 Ufasoft  http://ufasoft.com  mailto:support@ufasoft.com,  Sergey Pavlov  mailto:dev@ufasoft.com #######################################
#                                                                                                                                                                          #
# This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation;  #
# either version 3, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the      #
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU #
# General Public License along with this program; If not, see <http://www.gnu.org/licenses/>                                                                               #
##########################################################################################################################################################################*/

#pragma once

// Modular arithmetic

#include <el/num/num.h>

namespace Ext { namespace Num {

class ModNum {
public:
	BigInteger m_val;
	BigInteger m_mod;

	ModNum(const BigInteger& val, const BigInteger& mod)
		:	m_val(val)
		,	m_mod(mod)
	{}
};

ModNum inverse(const ModNum& mn);
ModNum pow(const ModNum& mn, const BigInteger& p);



}} // Ext::Num::

