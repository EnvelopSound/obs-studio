/*
Copyright (C) 2018 by pkv <pkv.stream@gmail.com>, andersama <anderson.john.alexander@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* For full GPL v2 compatibility it is required to build libs with
 * our open source sdk instead of steinberg sdk , see our fork:
 * https://github.com/pkviet/portaudio , branch : openasio
 * If you build with original asio sdk, you are free to do so to the
 * extent that you do not distribute your binaries.
 */

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <vector>
#include <stdio.h>
#include <JuceHeader.h>
#include <juce_audio_processors/juce_audio_processors.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vst3", "en-US")

#define blog(level, msg, ...) blog(level, "obs-vst3: " msg, ##__VA_ARGS__)

// get number of output channels (this is set in obs general audio settings
int get_obs_output_channels()
{
	// get channel number from output speaker layout set by obs
	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	return (int)get_audio_channels(aoi.speakers);
}

int get_max_obs_channels()
{
	static int channels = 0;
	if (channels > 0) {
		return channels;
	} else {
		for (int i = 0; i < 1024; i++) {
			int c = get_audio_channels((speaker_layout)i);
			if (c > channels)
				channels = c;
		}
		return channels;
	}
}

VST3PluginFormat     vst3format;
const int            obs_output_frames = AUDIO_OUTPUT_FRAMES;
const FileSearchPath search            = vst3format.getDefaultLocationsToSearch();
StringArray          paths             = vst3format.searchPathsForPlugins(search, true, true);
const int            obs_max_channels  = get_max_obs_channels();

class VST3Host {
private:
	juce::OwnedArray<juce::PluginDescription> descs;
	PluginDescription                         desc;
	AudioPluginInstance *                     vst_instance     = nullptr;
	AudioPluginInstance *                     new_vst_instance = nullptr;
	AudioPluginInstance *                     old_vst_instance = nullptr;
	AudioProcessorEditor *                    editor           = nullptr;
	obs_source_t *                            context          = nullptr;
	juce::String                              current_file;

	juce::MidiBuffer         midi;
	juce::AudioBuffer<float> buffer;

	bool enable = true;
	bool swap   = false;

	void update(obs_data_t *settings)
	{
		if (old_vst_instance) {
			old_vst_instance->releaseResources();
			delete old_vst_instance;
			old_vst_instance = nullptr;
		}
		obs_audio_info aoi;
		juce::String   file = obs_data_get_string(settings, "effect");
		juce::String   err;
		if (file.compare(current_file) != 0) {
			if (editor) {
				delete editor;
				editor = nullptr;
			}
			vst3format.findAllTypesForFile(descs, file);
			if (descs.size() > 0) {
				blog(LOG_INFO, "%s", descs[0]->name.toStdString().c_str());
				desc = *descs[0];
				if (obs_get_audio_info(&aoi)) {
					new_vst_instance =
							vst3format.createInstanceFromDescription(desc,
									aoi.samples_per_sec, 2 * obs_output_frames,
									err);
					if (err.toStdString().length() > 0) {
						blog(LOG_WARNING, "%s", err.toStdString().c_str());
					}
					if (new_vst_instance) {
						new_vst_instance->prepareToPlay(
								(double)aoi.samples_per_sec, 2 * obs_output_frames);
						new_vst_instance->setNonRealtime(false);
						//AudioProcessorListener *bypass_listener = new AudioProcessorListener();
						//AudioProcessorListener *       l = new AudioProcessorListener();
						//juce::AudioProcessorParameter *param =
						//		new_vst_instance->getBypassParameter();
						//param->addListener(l)
					}
				}
			}
			swap = true;
		}

		enable = obs_data_get_bool(settings, "bypass");
		/*
		if (vst_instance) {
			juce::AudioProcessorParameter *param = vst_instance->getBypassParameter();
			param->setValue(enable);
		}
		if (new_vst_instance) {
			juce::AudioProcessorParameter *param = new_vst_instance->getBypassParameter();
			param->setValue(enable);
		}
		*/
	}

	void save(obs_data_t *settings)
	{
	}

	void filter_audio(struct obs_audio_data *audio)
	{
		if (swap) {
			old_vst_instance = vst_instance;
			vst_instance = new_vst_instance;
			new_vst_instance = nullptr;
			swap             = false;
		}

		/*Process w/ VST*/
		if (vst_instance) {
			int chs = 0;
			for (; chs < obs_max_channels; chs++)
				if (!audio->data[chs])
					break;

			struct obs_audio_info aoi;
			obs_get_audio_info(&aoi);

			// vst_instance->prepareToPlay((double)aoi.samples_per_sec, audio->frames);
			buffer.setDataToReferTo((float **)audio->data, chs, audio->frames);
			if (enable)
				vst_instance->processBlock(buffer, midi);
			else
				vst_instance->processBlockBypassed(buffer, midi);
			/* do we need this? */
			float **out = buffer.getArrayOfWritePointers();
			for (int i = 0; i < chs; i++)
				memcpy(audio->data[i], out[i], audio->frames * sizeof(float));
		}
	}

public:
	VST3Host(obs_data_t *settings, obs_source_t *source) : context(source)
	{
		update(settings);
	}

	~VST3Host()
	{
		if (editor) {
			delete editor;
			editor = nullptr;
		}
		if (vst_instance) {
			vst_instance->releaseResources();
			delete vst_instance;
		}
		if (old_vst_instance) {
			old_vst_instance->releaseResources();
			delete old_vst_instance;
		}
		if (new_vst_instance) {
			new_vst_instance->releaseResources();
			delete new_vst_instance;
		}
	}

	void host_clicked()
	{
		if (editor) {
			delete editor;
			editor = nullptr;
		}

		if (has_gui()) {
			//AudioProcessorEditor *p = vst_instance->createEditor();
			editor = vst_instance->createEditorIfNeeded();
			if (editor) {
				editor->addToDesktop(ComponentPeer::StyleFlags::windowHasCloseButton |
						ComponentPeer::StyleFlags::windowHasTitleBar |
						ComponentPeer::StyleFlags::windowHasMinimiseButton |
						ComponentPeer::StyleFlags::windowIsResizable);
				editor->setResizable(true, true);
				editor->setAlpha(1.0);
				editor->setVisible(true);
				
				/*
						*/
			}
			// vst_instance->createEditorIfNeeded();
		}
	}

	bool has_gui()
	{
		return vst_instance && vst_instance->hasEditor();
	}

	static bool vst_host_clicked(obs_properties_t *props, obs_property_t *property, void *vptr)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		if (plugin)
			plugin->host_clicked();
		return false;
	}

	static bool vst_selected_modified(void *vptr, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		obs_property_t *vst_host_button = obs_properties_get(props, "vst_button");
		obs_property_set_enabled(vst_host_button, plugin && plugin->has_gui());
		return false;
	}

	static obs_properties_t *Properties(void *vptr)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);

		obs_properties_t *props;
		props = obs_properties_create();

		obs_property_t *vst_list;
		obs_property_t *vst_host_button;
		obs_property_t *bypass;
		vst_list = obs_properties_add_list(
				props, "effect", "vsts", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_set_modified_callback2(vst_list, vst_selected_modified, plugin);

		vst_host_button = obs_properties_add_button2(props, "vst_button", "Show", vst_host_clicked, plugin);
		obs_property_set_enabled(vst_host_button, plugin->has_gui());
		obs_properties_add_bool(props, "bypass", "enable effect");
		

		/*Add VSTs to list*/
		bool scannable = vst3format.canScanForPlugins();
		if (scannable) {
			/*
			const FileSearchPath search = vst3format.getDefaultLocationsToSearch();
			StringArray paths = vst3format.searchPathsForPlugins(search, true, true);
			*/
			if (paths.size() < 1)
				paths = vst3format.searchPathsForPlugins(search, true, true);
			for (int i = 0; i < paths.size(); i++) {
				juce::String name = vst3format.getNameOfPluginFromIdentifier(paths[i]);
				obs_property_list_add_string(
						vst_list, paths[i].toStdString().c_str(), name.toStdString().c_str());
			}
		}

		return props;
	}

	static void Update(void *vptr, obs_data_t *settings)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		if (plugin)
			plugin->update(settings);
	}

	static void Defaults(obs_data_t *settings)
	{
		/*Setup Defaults*/
		obs_data_set_default_string(settings, "effect", "None");
	}

	static const char *Name(void *unused)
	{
		UNUSED_PARAMETER(unused);
		return obs_module_text("Vst3Plugin");
	}

	static void *Create(obs_data_t *settings, obs_source_t *source)
	{
		return new VST3Host(settings, source);
	}

	static void Save(void *vptr, obs_data_t *settings)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		if (plugin)
			plugin->save(settings);
	}

	static void Destroy(void *vptr)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		delete plugin;
		plugin = nullptr;
	}

	static struct obs_audio_data *Filter_Audio(void *vptr, struct obs_audio_data *audio)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		plugin->filter_audio(audio);

		return audio;
	}
};

bool obs_module_load(void)
{
	struct obs_source_info vst3_filter = {};
	vst3_filter.id                     = "vst_filter3";
	vst3_filter.type                   = OBS_SOURCE_TYPE_FILTER;
	vst3_filter.output_flags           = OBS_SOURCE_AUDIO;
	vst3_filter.get_name               = VST3Host::Name;
	vst3_filter.create                 = VST3Host::Create;
	vst3_filter.destroy                = VST3Host::Destroy;
	vst3_filter.update                 = VST3Host::Update;
	vst3_filter.filter_audio           = VST3Host::Filter_Audio;
	vst3_filter.get_properties         = VST3Host::Properties;
	vst3_filter.save                   = VST3Host::Save;

	obs_register_source(&vst3_filter);
	return true;
}
