/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "d3d12_impl_device.hpp"
#include "d3d12_impl_command_queue.hpp"
#include "d3d12_impl_swapchain.hpp"
#include "d3d12_impl_type_convert.hpp"
#include "dll_log.hpp" // Include late to get HRESULT log overloads
#include "addon_manager.hpp"
#include <CoreWindow.h>

reshade::d3d12::swapchain_impl::swapchain_impl(device_impl *device, command_queue_impl *queue, IDXGISwapChain3 *swapchain) :
	api_object_impl(swapchain, device, queue)
{
	_renderer_id = D3D_FEATURE_LEVEL_12_0;

	// There is no swap chain in d3d12on7
	if (com_ptr<IDXGIFactory4> factory;
		_orig != nullptr && SUCCEEDED(_orig->GetParent(IID_PPV_ARGS(&factory))))
	{
		const LUID luid = device->_orig->GetAdapterLuid();

		if (com_ptr<IDXGIAdapter> dxgi_adapter;
			SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&dxgi_adapter))))
		{
			if (DXGI_ADAPTER_DESC desc; SUCCEEDED(dxgi_adapter->GetDesc(&desc)))
			{
				_vendor_id = desc.VendorId;
				_device_id = desc.DeviceId;

				LOG(INFO) << "Running on " << desc.Description << '.';
			}
		}
	}

	if (_orig != nullptr)
		on_init();
}
reshade::d3d12::swapchain_impl::~swapchain_impl()
{
	on_reset();
}

reshade::api::resource reshade::d3d12::swapchain_impl::get_back_buffer(uint32_t index)
{
	return to_handle(_back_buffers[index].get());
}

uint32_t reshade::d3d12::swapchain_impl::get_back_buffer_count() const
{
	return static_cast<uint32_t>(_back_buffers.size());
}
uint32_t reshade::d3d12::swapchain_impl::get_current_back_buffer_index() const
{
	assert(_orig != nullptr);

	return _orig->GetCurrentBackBufferIndex();
}

void reshade::d3d12::swapchain_impl::set_back_buffer_color_space(DXGI_COLOR_SPACE_TYPE type)
{
	_back_buffer_color_space = convert_color_space(type);
}

bool reshade::d3d12::swapchain_impl::on_init()
{
	assert(_orig != nullptr);

	DXGI_SWAP_CHAIN_DESC swap_desc;
	// Get description from 'IDXGISwapChain' interface, since later versions are slightly different
	if (FAILED(_orig->GetDesc(&swap_desc)))
		return false;

	// Update window handle in swap chain description for UWP applications
	if (HWND hwnd = nullptr; SUCCEEDED(_orig->GetHwnd(&hwnd)))
		swap_desc.OutputWindow = hwnd;
	else if (com_ptr<ICoreWindowInterop> window_interop; // Get window handle of the core window
		SUCCEEDED(_orig->GetCoreWindow(IID_PPV_ARGS(&window_interop))) && SUCCEEDED(window_interop->get_WindowHandle(&hwnd)))
		swap_desc.OutputWindow = hwnd;

	if (swap_desc.SampleDesc.Count > 1)
	{
		LOG(WARN) << "Multisampled swap chains are unsupported with D3D12.";
		return false;
	}

	// Get back buffer textures
	_back_buffers.resize(swap_desc.BufferCount);
	for (UINT i = 0; i < swap_desc.BufferCount; ++i)
	{
		if (FAILED(_orig->GetBuffer(i, IID_PPV_ARGS(&_back_buffers[i]))))
			return false;
		assert(_back_buffers[i] != nullptr);
	}

	assert(swap_desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT);

#if RESHADE_ADDON
	invoke_addon_event<addon_event::init_swapchain>(this);
#endif

	return runtime::on_init(swap_desc.OutputWindow);
}
void reshade::d3d12::swapchain_impl::on_reset()
{
	if (_back_buffers.empty())
		return;

	runtime::on_reset();

#if RESHADE_ADDON
	invoke_addon_event<addon_event::destroy_swapchain>(this);
#endif

	// Make sure none of the resources below are currently in use (in case the runtime was initialized previously)
	_graphics_queue->wait_idle();

	_back_buffers.clear();
}

reshade::d3d12::swapchain_d3d12on7_impl::swapchain_d3d12on7_impl(device_impl *device, command_queue_impl *queue) : swapchain_impl(device, queue, nullptr)
{
	// Default to three back buffers for d3d12on7
	_back_buffers.resize(3);
}

uint32_t reshade::d3d12::swapchain_d3d12on7_impl::get_current_back_buffer_index() const
{
	return _swap_index;
}

bool reshade::d3d12::swapchain_d3d12on7_impl::on_present(ID3D12Resource *source, HWND hwnd)
{
	assert(source != nullptr);

	_swap_index = (_swap_index + 1) % static_cast<UINT>(_back_buffers.size());

	// Update source texture render target view
	if (_back_buffers[_swap_index] != source)
	{
		runtime::on_reset();

#if RESHADE_ADDON
		if (_back_buffers[0] != nullptr)
		{
			invoke_addon_event<addon_event::destroy_swapchain>(this);
		}
#endif

		// Reduce number of back buffers if less are used than predicted
		if (const auto it = std::find(_back_buffers.begin(), _back_buffers.end(), source); it != _back_buffers.end())
			_back_buffers.erase(it);
		else
			_back_buffers[_swap_index] = source;

		// Do not initialize before all back buffers have been set
		// The first to be set is at index 1 due to the addition above, so it is sufficient to check the last to be set, which will be at index 0
		if (_back_buffers[0] != nullptr)
		{
#if RESHADE_ADDON
			invoke_addon_event<addon_event::init_swapchain>(this);
#endif

			if (!runtime::on_init(hwnd))
				return false;
		}
	}

	// Is not initialized the first few frames, but that is fine, since 'on_present' does an 'is_initialized' check
	runtime::on_present();
	return true;
}
