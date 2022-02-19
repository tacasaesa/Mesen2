﻿using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Dock.Model.ReactiveUI.Controls;
using Mesen.Config;
using Mesen.Debugger.Labels;
using Mesen.Debugger.Utilities;
using Mesen.Debugger.Windows;
using Mesen.Interop;
using Mesen.ViewModels;
using ReactiveUI.Fody.Helpers;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace Mesen.Debugger.ViewModels
{
	public class CallStackViewModel : ViewModelBase
	{
		public CpuType CpuType { get; }
		public DisassemblyViewModel Disassembly { get; }

		[Reactive] public List<StackInfo> CallStackContent { get; private set; } = new List<StackInfo>();

		private StackFrameInfo[] _stackFrames = Array.Empty<StackFrameInfo>();

		[Obsolete("For designer only")]
		public CallStackViewModel() : this(CpuType.Snes, new()) { }

		public CallStackViewModel(CpuType cpuType, DisassemblyViewModel disassembly)
		{
			Disassembly = disassembly;
			CpuType = cpuType;
			
			if(Design.IsDesignMode) {
				return;
			}

			UpdateCallStack();
		}

		public void UpdateCallStack()
		{
			_stackFrames = DebugApi.GetCallstack(CpuType);
			RefreshCallStack();
		}

		public void RefreshCallStack()
		{
			CallStackContent = GetStackInfo();
		}

		private List<StackInfo> GetStackInfo()
		{
			StackFrameInfo[] stackFrames = _stackFrames;

			List<StackInfo> stack = new List<StackInfo>();
			for(int i = 0; i < stackFrames.Length; i++) {
				bool isMapped = DebugApi.GetRelativeAddress(stackFrames[i].AbsSource, CpuType).Address >= 0;
				stack.Insert(0, new StackInfo() {
					EntryPoint = GetEntryPoint(i == 0 ? null : stackFrames[i - 1]),
					EntryPointAddr = i == 0 ? null : stackFrames[i - 1].AbsTarget,
					RelAddress = stackFrames[i].Source,
					Address = stackFrames[i].AbsSource,
					RowBrush = isMapped ? AvaloniaProperty.UnsetValue : Brushes.Gray,
					RowStyle = isMapped ? FontStyle.Normal : FontStyle.Italic
				});
			}

			//Add current location
			stack.Insert(0, new StackInfo() {
				EntryPoint = GetEntryPoint(stackFrames.Length > 0 ? stackFrames[^1] : null),
				EntryPointAddr = stackFrames.Length > 0 ? stackFrames[^1].AbsTarget : null,
				RelAddress = DebugUtilities.GetProgramCounter(CpuType),
				Address = DebugApi.GetAbsoluteAddress(new AddressInfo() { Address = (int)DebugUtilities.GetProgramCounter(CpuType), Type = CpuType.ToMemoryType() })
			});

			return stack;
		}

		private string GetEntryPoint(StackFrameInfo? stackFrame)
		{
			if(stackFrame == null) {
				return "[bottom of stack]";
			}

			StackFrameInfo entry = stackFrame.Value;

			string format = "X" + CpuType.GetAddressSize();
			CodeLabel? label = LabelManager.GetLabel(entry.AbsTarget);
			if(label != null) {
				return label.Label + " ($" + entry.Target.ToString(format) + ")";
			} else if(entry.Flags == StackFrameFlags.Nmi) {
				return "[nmi] $" + entry.Target.ToString(format);
			} else if(entry.Flags == StackFrameFlags.Irq) {
				return "[irq] $" + entry.Target.ToString(format);
			}

			return "$" + entry.Target.ToString(format);
		}

		private bool IsMapped(StackInfo entry)
		{
			return DebugApi.GetRelativeAddress(entry.Address, CpuType).Address >= 0;
		}

		public void GoToLocation(StackInfo entry)
		{
			if(IsMapped(entry)) {
				Disassembly.ScrollToAddress(entry.RelAddress);
			}
		}

		public void InitContextMenu(Control parent, DataGrid grid)
		{
			DebugShortcutManager.CreateContextMenu(parent, new object[] {
				new ContextMenuAction() {
					ActionType = ActionType.EditLabel,
					Shortcut = () => ConfigManager.Config.Debug.Shortcuts.Get(DebuggerShortcut.CallStack_EditLabel),
					IsEnabled = () => grid.SelectedItem is StackInfo entry && entry.EntryPointAddr != null,
					OnClick = () => {
						if(grid.SelectedItem is StackInfo entry && entry.EntryPointAddr != null) {
							LabelEditWindow.EditLabel(CpuType, parent, new CodeLabel() {
								Address = (uint)entry.EntryPointAddr.Value.Address,
								MemoryType = entry.EntryPointAddr.Value.Type
							});
						}
					}
				},

				new ContextMenuAction() {
					ActionType = ActionType.GoToLocation,
					Shortcut = () => ConfigManager.Config.Debug.Shortcuts.Get(DebuggerShortcut.CallStack_GoToLocation),
					IsEnabled = () => grid.SelectedItem is StackInfo entry && IsMapped(entry),
					OnClick = () => {
						if(grid.SelectedItem is StackInfo entry) {
							GoToLocation(entry);
						}
					}
				},
			});
		}

		public class StackInfo
		{
			public string EntryPoint { get; set; } = "";
			public AddressInfo? EntryPointAddr { get; set; }

			public UInt32 RelAddress { get; set; }
			public AddressInfo Address { get; set; }

			public object RowBrush { get; set; } = AvaloniaProperty.UnsetValue;
			public FontStyle RowStyle { get; set; }
		}
	}
}
