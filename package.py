# -*- coding: utf-8 -*-

name = 'hdcycles'

version = '0.7.21'

authors = [
    'benjamin.skinner',
]

requires = [
    'usdcycles',
    'cycles-1.13',
]


variants = [
    ['platform-windows', 'arch-x64', 'os-windows-10', 'usd-20.05-ta.1.2', 'openvdb-7.0.0'],
    ['platform-windows', 'arch-x64', 'os-windows-10', 'usd-19.11-houdini'],
    ['platform-windows', 'arch-x64', 'os-windows-10', 'usd-20.08-houdini', 'openvdb-7.1.0'],
]

build_system = "cmake"

# Release this as an internal package
with scope("config") as c:
    import sys
    if 'win' in str(sys.platform):
        c.release_packages_path = "R:/int"
    else:
        c.release_packages_path = "/r/int"

@early()
def private_build_requires():
    import sys
    if 'win' in str(sys.platform):
        return ['visual_studio']
    else:
        return ['gcc-7']

def commands():        
    env.HDCYCLES_ROOT.set('{root}')
    env.HDCYCLES_PLUGIN_ROOT.set('{root}/plugin')
    env.HDCYCLES_TOOLS_ROOT.set('{root}/tools')

    env.PXR_PLUGINPATH_NAME.append('{0}/usd/ndrCycles/resources'.format(env.HDCYCLES_PLUGIN_ROOT))
    env.PXR_PLUGINPATH_NAME.append('{0}/usd/hdCycles/resources'.format(env.HDCYCLES_PLUGIN_ROOT))

    env.PATH.append("{0}".format(env.HDCYCLES_TOOLS_ROOT))