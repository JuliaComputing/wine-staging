/*
 * Copyright 2020 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef _ROERROR_H
#define _ROERROR_H

#include <hstring.h>
#include <restrictederrorinfo.h>
#include <rpc.h>

HRESULT WINAPI GetRestrictedErrorInfo(IRestrictedErrorInfo **info);
BOOL    WINAPI RoOriginateError(HRESULT error, HSTRING message);
BOOL    WINAPI RoOriginateLanguageException(HRESULT error, HSTRING message, IUnknown *language_exception);

#endif /* _ROERROR_H */
