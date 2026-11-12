#!/usr/bin/env python3
from pathlib import Path
import json
import re
for p in Path('kde').rglob('*'):
    if not p.is_file(): continue
    assert p.suffix != '.json', f'Duplicated schema: {p}'
    if p.suffix not in ('.cpp','.h'): continue
    code='\n'.join(line for line in p.read_text().splitlines() if not line.lstrip().startswith('//'))
    assert 'config.toml' not in code, f'KDE must use resolved backend config: {p}'
    assert not re.search(r'"--(?:compression|order|filter|block-size-bits)', code), p
assert not Path('Cargo.toml').exists()
print('Architecture guards passed')

# Fedora's release and architecture are shared policy, not literals repeated
# across entry points. Both native architectures must remain release-gated.
targets = Path('packaging/targets.sh').read_text()
assert 'FEDORA_RELEASE=${FEDORA_RELEASE:-45}' in targets
assert 'x86_64|aarch64' in targets
for rel in ['packaging/container-build.sh', 'packaging/test-package.sh',
            'packaging/install.sh']:
    text = Path(rel).read_text()
    assert '. packaging/targets.sh' in text, f'{rel} bypasses shared Fedora policy'
    assert 'fedora:44' not in text and 'fc44' not in text
release = Path('packaging/release-artifacts.sh').read_text()
assert 'fc45.x86_64.rpm' in release and 'fc45.aarch64.rpm' in release
assert 'gpg --yes --armor --detach-sign' in release
assert 'SHA256SUMS.asc' in release and 'SHA256SUMS.sig' not in release
assert Path('RELEASE_SIGNING_KEY.asc').is_file()
assert not Path('RELEASE_SIGNING_KEY.pub').exists()
workflow = Path('.github/workflows/ci.yml').read_text()
assert 'ubuntu-24.04-arm' in workflow and 'arch: aarch64' in workflow
print('Fedora 45 multi-architecture package contract passed')
# MIME icon and open handler must be installed and agree with the MIME definition.
import configparser, xml.etree.ElementTree as ET
mime=ET.parse('kde/mime/packages/application-x-dwarfs.xml')
ns={'m':'http://www.freedesktop.org/standards/shared-mime-info'}
type_node=mime.find('m:mime-type',ns)
icon=type_node.find('m:icon',ns).attrib['name']
glob=type_node.find('m:glob',ns)
magic=type_node.find('m:magic/m:match',ns)
assert glob.attrib=={'pattern':'*.dwarfs','weight':'80'}
assert magic.attrib['offset']=='0' and magic.attrib['value']=='DWARFS'
assert Path(f'kde/mime/icons/{icon}.svg').is_file()
ET.parse(f'kde/mime/icons/{icon}.svg')
entry=configparser.ConfigParser(interpolation=None)
entry.read('kde/open-image/dwarfsctl-open.desktop')
assert type_node.attrib['type'] in entry['Desktop Entry']['MimeType'].split(';')
assert entry['Desktop Entry']['Exec']=='dwarfsctl-open %f'
assert entry['Desktop Entry']['Icon']=='dwarfsctl'
assert Path('kde/tray/resources/dwarfsctl-app.svg').is_file()
assert Path('kde/tray/resources/dwarfsctl-volume.svg').is_file()
ET.parse('kde/tray/resources/dwarfsctl-app.svg')
tray_cmake = Path('kde/tray/CMakeLists.txt').read_text()
tray_qrc = Path('kde/tray/tray.qrc').read_text()
assert 'install(FILES resources/dwarfsctl-app.svg' in tray_cmake
assert 'RENAME "dwarfsctl.svg"' in tray_cmake
assert '<file>resources/dwarfsctl-app.svg</file>' in tray_qrc
print('MIME icon and open-handler contract passed')

# The pinned engine supplies mount, inspection, and extraction. Compact tray
# status replies intentionally omit dependency diagnostics, so front ends must
# not disable those operations based on absent `deps` fields.
for rel in [
    'kde/kf6-plugin/src/dwarfsfileitemactionplugin.cpp',
    'kde/kf6-plugin/src/dwarfspropertiesplugin.cpp',
]:
    text = Path(rel).read_text()
    assert 'setEnabled(depFound' not in text, f'stale dependency gating in {rel}'
print('Pinned-engine action enablement contract passed')

# The standalone utility is additive: opening a .dwarfs file still uses the
# small mount handler, while the utility reuses the existing KDE components and
# delegates mount ownership to the tray service.
manager_cmake = Path('kde/manager/CMakeLists.txt').read_text()
manager_source = Path('kde/manager/src/managerwindow.cpp').read_text()
manager_entry = configparser.ConfigParser(interpolation=None)
manager_entry.read('kde/manager/org.quest.entropy.dwarfsctl.Manager.desktop')
manager_desktop = manager_entry['Desktop Entry']
assert manager_desktop['Exec'] == 'dwarfsctl-manager %F'
assert manager_desktop['Icon'] == 'dwarfsctl'
assert type_node.attrib['type'] in manager_desktop['MimeType'].split(';')
assert entry['Desktop Entry']['Exec'] == 'dwarfsctl-open %f', 'manager replaced the default mount handler'
for shared_source in ['builddialog.cpp', 'integritycheckdialog.cpp',
                      'checksumdialog.cpp', 'settingsdialog.cpp',
                      'mountscache.cpp', 'trayagentjob.cpp']:
    assert shared_source in manager_cmake, f'manager forked shared component: {shared_source}'
assert 'TrayAgentJob' in manager_source and 'DwarfsctlJob' in manager_source
visualizations = Path('kde/manager/src/visualizationwidgets.cpp').read_text()
operations = Path('cpp/cli/operations.cpp').read_text()
assert 'QStringLiteral("usage")' in manager_source
assert 'dwarfsctl.usage.v1' in operations and 'kUsageMaxChildren' in operations
assert 'Whole-image compression' in visualizations and 'Content Rings' in visualizations
assert 'category' not in visualizations.lower(), 'manager invented per-category compression attribution'
assert 'for part in tray kf6-plugin manager thumbnailer' in Path('packaging/build-all.sh').read_text()
print('Additive DwarFS Utility component-sharing contract passed')

# Dolphin Properties remains the contextual information surface. Keep the
# context menu operational, the advanced dialog diagnostic, and the tray on one
# interaction model so retired parallel plugin interfaces cannot quietly return.
plugin_src = Path('kde/kf6-plugin/src')
for retired in ['statusdialog.cpp', 'statusdialog.h', 'peekdialog.cpp',
                'peekdialog.h', 'peeksummary.cpp', 'peeksummary.h']:
    assert not (plugin_src / retired).exists(), f'retired UI returned: {retired}'
file_actions = (plugin_src / 'dwarfsfileitemactionplugin.cpp').read_text()
assert 'Image Details' not in file_actions
properties = (plugin_src / 'dwarfspropertiesplugin.cpp').read_text()
for duplicate_action in ['More Actions', 'Check Image', 'Extract…', 'i18n("Unmount")']:
    assert duplicate_action not in properties, f'duplicate Properties action: {duplicate_action}'
advanced = (plugin_src / 'statusadvanceddialog.cpp').read_text()
for friendly_field in ['Original size', 'Created by', 'Format version',
                       'i18n("Files")', 'i18n("Folders")']:
    assert friendly_field not in advanced, f'friendly metadata duplicated in diagnostics: {friendly_field}'
tray = Path('kde/tray/src/main.cpp').read_text()
assert 'm_leftMenu' not in tray and 'rebuildLeftMenu' not in tray
print('Streamlined desktop integration contract passed')

# The tray owns one settings entry point, while the backend owns persistence
# and validation. Creation consumes those defaults rather than growing another
# independent settings store.
settings = Path('kde/tray/src/settingsdialog.cpp').read_text()
assert 'config.toml' not in settings
assert 'QStringLiteral("config"), QStringLiteral("set")' in settings
assert 'Remember Recent Images' not in tray
assert 'Settings…' in tray
assert 'creation.default_mode' in Path('cpp/cli/config.cpp').read_text()
assert 'loadCreationDefaults' in Path('kde/kf6-plugin/src/builddialog.cpp').read_text()
assert 'Tool payload pages' not in advanced
for page in ['Overview', 'Storage', 'Runtime', 'Report']:
    assert f'i18n("{page}")' in advanced
print('Unified settings and polished diagnostics contract passed')

# Preserve the custom About artwork that remains useful, without bringing back
# the ornamental divider that competed with the standard dialog layout.
about = Path('kde/tray/src/aboutdialog.cpp').read_text()
about_qrc = Path('kde/tray/tray.qrc').read_text()
assert 'dwarfsctl-divider' not in about and 'dwarfsctl-divider' not in about_qrc
assert not Path('kde/tray/resources/about/dwarfsctl-divider.png').exists()
for icon in ['github-icon.png', 'gplv3plus-icon.png']:
    assert icon in about and icon in about_qrc
    assert Path('kde/tray/resources/about', icon).is_file()
assert 'setAccessibleName' in about
assert 'setCurrentPage(licenseItem)' in about
assert 'setFixedSize(52, 52)' in about and 'setIconSize(QSize(44, 44))' in about
print('About artwork and link controls contract passed')

# Keep the deliberately small artwork set accountable. Adding an icon requires
# naming its desktop role here; removing a role requires removing its asset.
icon_assets = {
    str(path) for path in Path('kde').rglob('*')
    if path.is_file() and path.suffix.lower() in {'.png', '.svg'}
}
expected_icon_assets = {
    'kde/mime/icons/dwarfsctlimage.svg',
    'kde/tray/resources/about/dwarfsctl_about.png',
    'kde/tray/resources/about/github-icon.png',
    'kde/tray/resources/about/gplv3plus-icon.png',
    'kde/tray/resources/dwarfsctl-app.svg',
    'kde/tray/resources/dwarfsctl-volume.svg',
    'kde/tray/resources/helmet-mounted.svg',
    'kde/tray/resources/helmet-unmounted.svg',
}
assert icon_assets == expected_icon_assets, (
    f'unaccounted icon assets: added={sorted(icon_assets - expected_icon_assets)}, '
    f'missing={sorted(expected_icon_assets - icon_assets)}'
)
print('Icon asset inventory contract passed')

# Compression policy and profile authoring live in the tray Settings control
# plane. The per-build dialog shows the resolved choice and only retains
# destination plus build-level operational options.
build_dialog = Path('kde/kf6-plugin/src/builddialog.cpp').read_text()
for retired_control in ['ProfileEditorDialog', 'recommendAuto', 'recommendGoal',
                        'recommendUse', 'recommendSummary', 'View…', 'Edit…']:
    assert retired_control not in build_dialog, f'creation control returned: {retired_control}'
assert 'compressionStatus' in build_dialog
assert 'Change compression defaults and profiles from the DwarFS tray Settings.' in build_dialog
assert 'Optimized for %1' in build_dialog and 'confidence' not in build_dialog.split('compressionStatus->setText', 1)[-1]
assert 'ProfileEditorDialog editor' in settings
assert 'Create or Edit Profiles…' in settings
for everyday_control in ['Optimize for:', 'Balanced (recommended)', 'Advanced User',
                          'Diagnostics', 'settingsAdvancedUserToggle']:
    assert everyday_control in settings
assert settings.count('tr("Advanced User")') == 1
assert 'advancedCreationToggle' not in settings
assert 'write_sidecar' not in settings and '.meta.json' not in settings
operations = Path('cpp/cli/operations.cpp').read_text()
assert '.meta.json' not in operations and 'sidecar_warning' not in operations
tray_cmake = Path('kde/tray/CMakeLists.txt').read_text()
assert '../kf6-plugin/src/profileeditordialog.cpp' in tray_cmake
profile_editor = Path('kde/kf6-plugin/src/profileeditordialog.cpp').read_text()
for progressive_control in ['simpleCompressionLevel', 'exactSettingsSummary',
                            'exactProfileSettings', 'advancedOptionsNavigation']:
    assert progressive_control in profile_editor
for retired_profile_gate in ['advancedUserToggle', 'profileModeStack']:
    assert retired_profile_gate not in profile_editor
schema = json.loads(Path('resources/mkdwarfs_schema_v0_15_7.json').read_text())
simple_controls = [
    option for category in schema['categories'] for option in category['options']
    if option.get('simple_control')
]
assert [(option['opt'], option['simple_control']) for option in simple_controls] == [
    ('--compress-level', 'compression-priority')
]
assert 'tst_profile_editor' in tray_cmake
plugin_cmake = Path('kde/kf6-plugin/CMakeLists.txt').read_text()
fileitem_sources = plugin_cmake.split('set(FILEITEM_PLUGIN_SOURCES', 1)[1].split(')', 1)[0]
assert 'profileeditordialog' not in fileitem_sources
print('Streamlined creation and profile control-plane contract passed')

# KF6 discovers property extensions in `kf6/propertiesdialog` and filters them
# using the current KPlugin MimeTypes field. A plausible-looking legacy service
# block in another directory compiles and packages but is never loaded.
plugin_cmake = Path('kde/kf6-plugin/CMakeLists.txt').read_text()
assert 'INSTALL_NAMESPACE "kf6/propertiesdialog"' in plugin_cmake
assert 'INSTALL_NAMESPACE "kf6/kpropertiesdialog"' not in plugin_cmake
properties_metadata = json.loads(
    Path('kde/kf6-plugin/src/dwarfspropertiesplugin.json.in')
    .read_text()
    .replace('@DWARFSCTL_DOLPHIN_VERSION@', '0.0.0')
)
assert set(properties_metadata['KPlugin']['MimeTypes']) == {
    'application/x-dwarfs', 'application/x-dwarfs-image'
}
assert properties_metadata['X-KDE-Protocols'] == ['file']
print('KF6 Properties plugin discovery contract passed')

thumbnail = Path('kde/thumbnailer/src/main.cpp').read_text()
assert 'QFont' not in thumbnail and 'drawText' not in thumbnail
build_all = Path('packaging/build-all.sh').read_text()
assert 'hicolor/${size}x${size}/mimetypes/dwarfsctlimage.svg' in build_all
print('D-Mark thumbnail and fixed-size icon fallback contract passed')
