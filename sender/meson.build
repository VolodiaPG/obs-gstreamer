#
# obs-gstreamer. OBS Studio plugin.
# Copyright (C) 2021 Volodia PAROL-GUARINO
#
# This file is part of stream-in-sync.
#
# stream-in-sync is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# obs-gstreamer is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
#

project('stream-in-sync-sender', 'c')

executable('sender',
  'log.c',
  'sender.c',
  'main.c',
  vcs_tag(
    command : ['git', 'rev-parse', '--short', 'HEAD'],
    input : 'version.c.in',
    output : 'version.c',
  ),
  name_prefix : '',
  dependencies : [
    dependency('gstreamer-1.0', version : '>=1.16.0'),
    dependency('gstreamer-video-1.0'),
    dependency('gstreamer-audio-1.0'),
    dependency('gstreamer-app-1.0'),
    dependency('gstreamer-net-1.0'),
  ],
)
