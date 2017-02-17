#!/usr/bin/env python3

#
# __init__.py
#
# Copyright (C) 2016 Christian Hergert <chergert@redhat.com>
# Copyright (C) 2017 Georg Vienna <georg.vienna@himbarsoft.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import gi
import os
import re
import pty
import stat

gi.require_version('Ide', '1.0')
gi.require_version('Gtk', '3.0')

from gi.repository import GLib
from gi.repository import GObject
from gi.repository import Gio
from gi.repository import Gtk
from gi.repository import Ide
from gi.repository import Peas

_ = Ide.gettext

def get_module_data_path(name):
    engine = Peas.Engine.get_default()
    plugin = engine.get_plugin_info('rustup_plugin')
    data_dir = plugin.get_data_dir()
    return GLib.build_filenamev([data_dir, name])

class RustUpWorkbenchAddin(GObject.Object, Ide.WorkbenchAddin):
    """
    The RustUpWorkbenchAddin is a helper to handle open workbenches.
    It manages the set of open workbenches stored in the application addin.
    """
    def do_load(self, workbench):
        RustupApplicationAddin.instance.add_workbench(workbench)
        def unload(workbench, context):
            RustupApplicationAddin.instance.workbenches.discard(workbench)
        workbench.connect('unload', unload)

    def do_unload(self, workbench):
        RustupApplicationAddin.instance.workbenches.discard(workbench)

_NO_RUSTUP = _('Rustup not installed')

class RustupApplicationAddin(GObject.Object, Ide.ApplicationAddin):
    """
    The RustupApplicationAddin provides us a single point to manage updates
    within the Builder process. Our other addins will perform their various
    actions by communicating with this singleton.
    """
    # Our singleton instance
    instance = None

    __gsignals__ = {
        # emitted when a rustup installation is detected
        'rustup_changed': (GObject.SIGNAL_RUN_FIRST, None, ())
    }

    @GObject.Property(type=bool, default=False)
    def busy(self):
        return self.active_transfer is not None

    @GObject.Property(type=str, default=_NO_RUSTUP)
    def version(self):
        return self.rustup_version

    def do_load(self, application):
        RustupApplicationAddin.instance = self
        self.workbenches = set()
        self.active_transfer = None
        self.has_rustup = False
        self.rustup_version = _NO_RUSTUP
        self.rustup_executable = None
        self.check_rustup()

    def do_unload(self, application):
        RustupApplicationAddin.instance = None

    def check_rustup(self):
        """
            Checks if a rustup installation is available.
        """
        self.rustup_executable = None
        for rustup_bin in ['rustup', os.path.expanduser('~/.cargo/bin/rustup')]:
            try:
                launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.STDOUT_PIPE)
                launcher.push_argv(rustup_bin)
                launcher.push_argv('-V')
                launcher.set_run_on_host(True)
                sub_process = launcher.spawn()

                _, stdout, stderr = sub_process.communicate_utf8(None, None)
                self.rustup_executable = rustup_bin
                break
            except GLib.Error as e:
                pass
        if self.rustup_executable is None:
            self.has_rustup = False
            self.rustup_version = _NO_RUSTUP
        else:
            self.has_rustup = True
            self.rustup_version = stdout.replace('rustup','').strip()
        self.emit('rustup_changed')
        self.notify('version')

    def get_toolchains(self):
        if self.rustup_executable is None:
            return []
        try:
            launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.STDOUT_PIPE)
            launcher.push_argv(self.rustup_executable)
            launcher.push_argv('toolchain')
            launcher.push_argv('list')
            launcher.set_run_on_host(True)
            sub_process = launcher.spawn()

            _, stdout, stderr = sub_process.communicate_utf8(None, None)
            toolchains = []
            for line in iter(stdout.splitlines()):
                if not 'no installed toolchains' in line:
                    toolchains.append((line.strip(), '(default)' in line))
            return toolchains
        except Exception as e:
            pass
        return []

    def set_default(self, toolchain):
        if self.rustup_executable is None:
            return
        try:
            launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.STDOUT_PIPE)
            launcher.push_argv(self.rustup_executable)
            launcher.push_argv('default')
            launcher.push_argv(toolchain)
            launcher.set_run_on_host(True)
            sub_process = launcher.spawn()

            _, stdout, stderr = sub_process.communicate_utf8(None, None)
        except Exception as e:
            print(e)
            pass
        self.emit('rustup_changed')

    def install_toolchain(self, toolchain):
        self.run_transfer(RustupInstaller(toolchain=toolchain))

    def remove(self, toolchain):
        if self.rustup_executable is None:
            return
        try:
            toolchain = toolchain.replace('(default)', '').strip()
            launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.STDOUT_PIPE)
            launcher.push_argv(self.rustup_executable)
            launcher.push_argv('toolchain')
            launcher.push_argv('uninstall')
            launcher.push_argv(toolchain)
            launcher.set_run_on_host(True)
            sub_process = launcher.spawn()

            _, stdout, stderr = sub_process.communicate_utf8(None, None)
        except Exception as e:
            print(e)
            pass
        self.emit('rustup_changed')

    def add_workbench(self, workbench):
        # recheck if rustup was installed outside of gnome-builder
        def is_active(workbench, active):
            if workbench.is_active ():
                if self.active_transfer is None:
                    RustupApplicationAddin.instance.check_rustup()
        workbench.connect('notify::is-active', is_active)
        # call us if a transfer completes (could be the active_transfer)
        workbench.get_context().get_transfer_manager().connect('transfer-completed', self.transfer_completed)
        workbench.get_context().get_transfer_manager().connect('transfer-failed', self.transfer_failed)
        self.workbenches.add(workbench)
        # add the current transfer to the new workbench
        if self.active_transfer:
            workbench.get_context().get_transfer_manager().queue(self.active_transfer)

    def transfer_completed(self, transfer_manager, transfer):
        # reset the active transfer on completion, ensures that new workbenches dont get an old transfer
        if self.active_transfer == transfer:
            self.active_transfer = None
            self.notify('busy')

    def transfer_failed(self, transfer_manager, transfer, error):
        # reset the active transfer on error, ensures that new workbenches dont get an old transfer
        if self.active_transfer == transfer:
            self.active_transfer = None
            self.notify('busy')

    def run_transfer(self, transfer):
        self.active_transfer = transfer
        self.notify('busy')
        # run it in all transfer managers
        for workbench in self.workbenches:
            workbench.get_context().get_transfer_manager().queue(transfer)

    def install(self):
        self.run_transfer(RustupInstaller(mode=_MODE_INSTALL))

    def update(self):
        self.run_transfer(RustupInstaller(mode=_MODE_UPDATE))

_MODE_INSTALL = 0
_MODE_UPDATE = 1
_MODE_INSTALL_TOOLCHAIN = 2

_STATE_INIT = 0
_STATE_DOWN_SELF = 1
_STATE_SYNC_UPDATE = 2
_STATE_DOWN_COMP = 3
_STATE_INSTALL_COMP = 4
_STATE_CHECK_UPDATE_SELF = 5
_STATE_DOWN_UPDATE_SELF = 6

class RustupInstaller(Ide.Object, Ide.Transfer):
    """
    The RustupInstaller handles the installation of rustup, rustc, rust-std and cargo.
    """
    title = GObject.Property(type=str)
    icon_name = GObject.Property(type=str)
    progress = GObject.Property(type=float)
    status = GObject.Property(type=str)

    def __init__(self, *args, **kwargs):
        if 'mode' in kwargs:
            self.mode = kwargs['mode']
            del kwargs['mode']
        if 'toolchain' in kwargs:
            self.mode = _MODE_INSTALL_TOOLCHAIN
            self.toolchain = kwargs['toolchain']
            del kwargs['toolchain']
        super().__init__(*args, **kwargs)

    def do_execute_async(self, cancellable, callback, data):
        if self.mode == _MODE_INSTALL:
            self.props.title = _('Installing rustup')
        elif self.mode == _MODE_UPDATE:
            self.props.title = _('Updating rustup')
        elif self.mode == _MODE_INSTALL_TOOLCHAIN:
            self.props.title = _('Installing ') + self.toolchain 
        self.props.status = _('Checking system')
        self.props.icon_name = 'emblem-system-symbolic'
        self.state = _STATE_INIT
        self.downloaded_components = 0
        self.installed_components = 0

        task = Gio.Task.new(self, cancellable, callback)
        launcher = Ide.SubprocessLauncher()
        launcher.set_run_on_host(True)
        launcher.set_clear_env(False)
        if self.mode == _MODE_INSTALL:
            rustup_sh_path = get_module_data_path('resources/rustup.sh')
            # ensure that the script is executable
            st = os.stat(rustup_sh_path)
            os.chmod(rustup_sh_path, st.st_mode | stat.S_IEXEC)
            launcher.push_argv(rustup_sh_path)
            # install default toolchain automatically
            launcher.push_argv('-y')
        elif self.mode == _MODE_UPDATE:
            launcher.push_argv(RustupApplicationAddin.instance.rustup_executable)
            launcher.push_argv('update')
        elif self.mode == _MODE_INSTALL_TOOLCHAIN:
            launcher.push_argv(RustupApplicationAddin.instance.rustup_executable)
            launcher.push_argv('toolchain')
            launcher.push_argv('install')
            launcher.push_argv(self.toolchain)
        # rustup needs a tty to give us a progress bar
        (master_fd, slave_fd) = pty.openpty()
        launcher.take_stdin_fd (slave_fd)
        launcher.take_stdout_fd (slave_fd)
        launcher.take_stderr_fd (slave_fd)

        data_stream = Gio.DataInputStream.new(Gio.UnixInputStream.new(master_fd, False))
        # set it to ANY so the progress bars can be parsed
        data_stream.set_newline_type(Gio.DataStreamNewlineType.ANY)
        data_stream.read_line_async(GLib.PRIORITY_DEFAULT, cancellable, self._read_line_cb, cancellable)

        sub_process = launcher.spawn()

        sub_process.wait_async(cancellable, self._wait_cb, task)

    def _read_line_cb(self, data_stream, result, cancellable):
        try:
            line, length = data_stream.read_line_finish_utf8(result)
            if line is not None:
                if 'downloading installer' in line:
                    self.props.status = _('Downloading rustup-init')
                    self.props.progress = 0
                    self.props.icon_name = 'folder-download-symbolic'
                    self.state = _STATE_DOWN_SELF
                elif 'syncing channel updates' in line:
                    self.props.status = _('Syncing channel updates')
                    self.props.progress = 0
                    self.props.icon_name = 'emblem-system-symbolic'
                    self.state = _STATE_SYNC_UPDATE
                elif 'downloading component' in line:
                    m = re.search("downloading component '(.*)'", line)
                    self.props.status = _('Downloading ') + m.group(1)
                    self.props.progress = 0
                    self.props.icon_name = 'folder-download-symbolic'
                    self.downloaded_components += 1
                    self.state = _STATE_DOWN_COMP
                elif 'installing component' in line:
                    m = re.search("installing component '(.*)'", line)
                    self.props.status = _('Installing ') + m.group(1)
                    self.props.progress = self.installed_components/self.downloaded_components
                    self.props.icon_name = 'emblem-system-symbolic'
                    self.installed_components += 1
                    self.state = _STATE_INSTALL_COMP
                elif 'checking for self-updates' in line:
                    self.props.status = _('Checking for rustup updates')
                    self.props.progress = 0
                    self.props.icon_name = 'emblem-system-symbolic'
                    self.state = _STATE_CHECK_UPDATE_SELF
                elif 'downloading self-update' in line:
                    self.props.status = _('Downloading rustup update')
                    self.props.progress = 0
                    self.props.icon_name = 'folder-download-symbolic'
                    self.state = _STATE_DOWN_UPDATE_SELF
                elif 'installed' in line or 'updated' in line or 'unchanged' in line:
                    # prevent further line reading
                    return
                elif self.state == _STATE_DOWN_SELF:
                    # the first progress can be empty, skip it
                    if length > 0:
                        percent = line.replace('#', '').replace('%', '').strip()
                        try:
                            self.props.progress = float(percent)/100
                        except Exception as te:
                            print('_read_line_cb', self.state, line)
                elif self.state == _STATE_DOWN_COMP or self.state == _STATE_SYNC_UPDATE or self.state == _STATE_CHECK_UPDATE_SELF  or self.state == _STATE_DOWN_UPDATE_SELF:
                    # the first progress can be empty, skip it
                    if length > 0:
                        try:
                            m = re.search('\\( *([0-9]*) %\\)', line)
                            fpercent = float(m.group(1))
                            self.props.progress = fpercent/100
                        except Exception as te:
                            print('_read_line_cb', line, te)
                data_stream.read_line_async(GLib.PRIORITY_DEFAULT, cancellable, self._read_line_cb, cancellable)
        except Exception as ex:
            # ignore cancelled error
            if ex.code is not 19:
                print('_read_line_cb', ex)

    def _wait_cb(self, sub_process, result, task):
        try:
            sub_process.wait_check_finish(result)
            if sub_process.get_exit_status() == 0:
                task.return_boolean(True)
            else:
                if self.mode == _MODE_INSTALL_TOOLCHAIN:
                    self.props.status = _('Error installing ') + self.toolchain
                else:
                    self.props.status = _('Error')
                task.return_boolean(False)
                
        except Exception as ex:
            # cancelled error
            if ex.code is 19:
                self.props.status = _('Cancelled')
            else:
                print('_wait_cb', ex, ex.code)
            task.return_boolean(False)
        RustupApplicationAddin.instance.check_rustup()

    def do_execute_finish(self, task):
        if task.propagate_boolean():
            self.props.status = _('Finished')
        return True

class ModelItem(GObject.Object):
    def __init__(self, title, default):
        GObject.Object.__init__(self)
        self.title = title
        self.default = default

class RustupPreferencesAddin(GObject.Object, Ide.PreferencesAddin):
    """
       PreferencesAddin to display the installed rustup version and to change the rustup installation
    """
    def do_load(self, preferences):
        preferences.add_page('sdk', _('SDKs'), 550)
        preferences.add_list_group('sdk', 'rustup', _('Rustup'), Gtk.SelectionMode.NONE, 100)
        preferences.add_group('sdk', 'rustup_toolchains_edit', _('Rustup Toolchains'), 100)

        # rustup page: displays version and allows to install/update rustup
        rustup_custom = self.create_rustup_page()

        # rustup toolchains page: displays the installed toolchains, allows to set the default
        # toolchain, install new toolchains and remove a toolchain
        rustup_toolchain_custom = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=5, expand=True, visible=True)
        # list of toolchains
        rustup_toolchain_custom.pack_start(self.create_toolchain_listbox(), True, True, 0)
        # list controls: install, set default and remove
        rustup_toolchain_custom.pack_start(self.create_toolchain_listcontrols(), True, True, 0)
        # gets displayed if rustup is not installed
        rustup_toolchain_custom.pack_start(self.create_no_rustup_label(), True, True, 0)
        # gets displayed if no toolchain is currently installed
        rustup_toolchain_custom.pack_start(self.create_no_toolchain_view(), True, True, 0)
        
        self.ids = [
            preferences.add_custom('sdk', 'rustup', rustup_custom, None, 1000),
            preferences.add_custom('sdk', 'rustup_toolchains_edit', rustup_toolchain_custom, None, 1000),
        ]

    def do_unload(self, preferences):
        if self.ids:
            for id in self.ids:
                preferences.remove_id(id)

    def create_rustup_page(self):
        rustup_custom = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12, expand=True, visible=True)
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, expand=True, visible=True)
        title = Gtk.Label(halign='start', expand=True, visible=True, label='Rustup')
        subtitle = Gtk.Label(halign='start', expand=True, visible=True)
        subtitle.get_style_context().add_class('dim-label')
        subtitle.set_markup('<small>' + RustupApplicationAddin.instance.rustup_version + '</small>')
        vbox.pack_start(title, True, True, 0)
        vbox.pack_start(subtitle, True, True, 0)

        # detect rustup and display version
        def rustup_version_changed(applicationAddin, version):
            subtitle.set_markup('<small>' + applicationAddin.rustup_version + '</small>')
        RustupApplicationAddin.instance.connect('notify::version', rustup_version_changed)

        rustup_button = Gtk.Button(halign='end', valign='center', expand=True, visible=True)
        rustup_button.set_label(_('Update') if RustupApplicationAddin.instance.has_rustup else _('Install'))
    
        # set the button label to install or update if rustup is available
        def has_rustup_callback(applicationAddin):
            rustup_button.set_label(_('Update') if applicationAddin.has_rustup else _('Install'))
        RustupApplicationAddin.instance.connect('rustup_changed', has_rustup_callback)
        has_rustup_callback(RustupApplicationAddin.instance)

        # set the button label to installing/updating if we're busy
        def busy(applicationAddin, param):
            rustup_button.set_sensitive(not applicationAddin.busy)
            if applicationAddin.busy:
                rustup_button.set_label(_('Updating') if applicationAddin.active_transfer.mode == _MODE_UPDATE else _('Installing'))
        RustupApplicationAddin.instance.connect('notify::busy', busy)      

        # allow to install rustup if not detected or to update it
        def change_rustup(button):
            if RustupApplicationAddin.instance.has_rustup:
                RustupApplicationAddin.instance.update()
            else:   
                RustupApplicationAddin.instance.install()
        rustup_button.connect('clicked', change_rustup)

        rustup_custom.pack_start(vbox, True, True, 0)
        rustup_custom.pack_start(rustup_button, True, True, 0)
        return rustup_custom

    def create_toolchain_listbox(self):
        self.toolchain_listbox = Gtk.ListBox(visible=True)
        # adjust to preference list style
        self.toolchain_listbox.get_style_context().add_class('frame')
        self.toolchain_listbox.set_selection_mode(Gtk.SelectionMode.SINGLE)

        def _create_list_item(item, data):
            return Gtk.Label(item.title, halign='start', valign='center', expand=True, visible=True)

        self.store = Gio.ListStore.new(ModelItem)
        self.toolchain_listbox.bind_model(self.store, _create_list_item, None)
        # handle row de/selection
        def row_selected(listbox, row, self):
            if row is not None:
                item = self.store.get_item(row.get_index())
                self.default_toolchain_button.set_sensitive(not item.default)
                self.remove_toolchain.set_sensitive(True)
            else:                
                self.default_toolchain_button.set_sensitive(False)
                self.remove_toolchain.set_sensitive(False)
        self.toolchain_listbox.connect('row-selected', row_selected, self)

        # reload toolchains
        def has_rustup_callback(applicationAddin, preferenceAddin):
            toolchains = RustupApplicationAddin.instance.get_toolchains()
            self.toolchain_listbox.set_visible(len(toolchains) != 0)
            tcs = []
            for toolchain in toolchains:
                tcs.append(ModelItem(toolchain[0], toolchain[1]))
            old_len = self.store.get_n_items()
            self.store.splice(0, old_len, tcs)
        RustupApplicationAddin.instance.connect('rustup_changed', has_rustup_callback, self)
        has_rustup_callback(RustupApplicationAddin.instance, self)
        return self.toolchain_listbox

    def create_toolchain_listcontrols(self):
        list_control = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12, expand=True, visible=True)
        # remove toolchain button
        remove_icon = Gtk.Image.new_from_gicon(Gio.ThemedIcon(name='list-remove-symbolic'), Gtk.IconSize.BUTTON)
        self.remove_toolchain = Gtk.Button(image = remove_icon, halign='end', expand=False, visible=True)
        self.remove_toolchain.set_sensitive(False)
        list_control.pack_end(self.remove_toolchain, False, False, 0)
        
        def remove_toolchain_(button):
            index = self.toolchain_listbox.get_selected_row().get_index()
            item = self.store.get_item(index)
            RustupApplicationAddin.instance.remove(item.title)
        self.remove_toolchain.connect('clicked', remove_toolchain_)

        # add toolchain button
        add_icon = Gtk.Image.new_from_gicon(Gio.ThemedIcon(name='list-add-symbolic'), Gtk.IconSize.BUTTON)
        add_toolchain = Gtk.MenuButton(image = add_icon, halign='end', expand=False, visible=True)
        add_toolchain.props.focus_on_click = False
        add_toolchain.set_popover(self.create_install_popover())
        add_toolchain.set_sensitive(RustupApplicationAddin.instance.has_rustup)
        list_control.pack_end(add_toolchain, False, False, 0)

        # disable button if we're busy
        def busy(applicationAddin, param):
            add_toolchain.set_sensitive(not applicationAddin.busy)
        RustupApplicationAddin.instance.connect('notify::busy', busy) 

        # reload toolchains
        def has_rustup_callback(applicationAddin):
            add_toolchain.set_sensitive(applicationAddin.has_rustup)
            toolchains = applicationAddin.get_toolchains()
            list_control.set_visible(applicationAddin.has_rustup and len(toolchains) != 0)
        RustupApplicationAddin.instance.connect('rustup_changed', has_rustup_callback)
        has_rustup_callback(RustupApplicationAddin.instance)

        # set default toolchain button
        self.default_toolchain_button = Gtk.Button(halign='end', valign='start', expand=True, visible=True, label=_('Default'))
        self.default_toolchain_button.set_sensitive(False)
        list_control.pack_end(self.default_toolchain_button, False, False, 0)

        def set_default_toolchain(button):
            index = self.toolchain_listbox.get_selected_row().get_index()
            item = self.store.get_item(index)
            RustupApplicationAddin.instance.set_default(item.title)
            self.toolchain_listbox.select_row(self.toolchain_listbox.get_row_at_index(index))
            self.default_toolchain_button.set_sensitive(False)
        self.default_toolchain_button.connect('clicked', set_default_toolchain)

        return list_control

    def create_install_popover(self):
        popover = Gtk.Popover(visible=False)
        popover.set_border_width(6)
        def add(b):
            RustupApplicationAddin.instance.install_toolchain(entry.get_text())
            popover.popdown()
        hlist = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0, expand=True, visible=True)
        hlist.get_style_context().add_class('linked')
        entry = Gtk.Entry(visible=True)
        entry.set_can_focus(True)
        entry.props.tooltip_text = _('''Standard release channel toolchain names have the following form:
                        <channel>[-<date>][-<host>]

                        <channel>    = stable|beta|nightly|<version>
                        <date>          = YYYY-MM-DD
                        <host>          = <target-triple>''')
        entry.connect('activate', add)
        hlist.add(entry)

        add_button = Gtk.Button(visible=True, label=_('Add'))
        add_button.connect('clicked', add)
        hlist.add(add_button)
        popover.add(hlist)
        return popover

    def create_no_rustup_label(self):
        label = Gtk.Label(label='', visible=False)
        # update label visibility
        def has_rustup_callback(applicationAddin):
            label.set_visible(not applicationAddin.has_rustup)
            label.set_text(_('Install Rustup to manage toolchains here!'))
        RustupApplicationAddin.instance.connect('rustup_changed', has_rustup_callback)
        has_rustup_callback(RustupApplicationAddin.instance)
        return label

    def create_no_toolchain_view(self):
        hlist = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=5, expand=True, visible=False)
        label_start = Gtk.Label(label=_('No toolchain installed. Click'), visible=True)
        label_end = Gtk.Label(label=_('to add a new toolchain!'), visible=True)
        add_icon = Gtk.Image.new_from_gicon(Gio.ThemedIcon(name='list-add-symbolic'), Gtk.IconSize.BUTTON)
        add_toolchain = Gtk.MenuButton(image = add_icon, halign='end', expand=False, visible=True)
        add_toolchain.props.focus_on_click = False
        add_toolchain.set_popover(self.create_install_popover())
        # update view visibility
        def has_rustup_callback(applicationAddin):
            toolchains = applicationAddin.get_toolchains()
            hlist.set_visible(applicationAddin.has_rustup and len(toolchains) == 0)
        RustupApplicationAddin.instance.connect('rustup_changed', has_rustup_callback)
        has_rustup_callback(RustupApplicationAddin.instance)

        # disable add button if we're busy
        def busy(applicationAddin, param):
            add_toolchain.set_sensitive(not applicationAddin.busy)
        RustupApplicationAddin.instance.connect('notify::busy', busy)

        hlist.pack_start(label_start, False, False, 0)
        hlist.pack_start(add_toolchain, False, False, 0)
        hlist.pack_start(label_end, False, False, 0)
        return hlist
