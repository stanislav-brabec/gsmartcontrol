/**************************************************************************
 Copyright:
      (C) 2008 - 2011  Alexander Shaduri <ashaduri 'at' gmail.com>
 License: See LICENSE_gsmartcontrol.txt
***************************************************************************/

#include <glibmm.h>  // Glib::shell_quote()

#include "rconfig/rconfig_mini.h"
#include "hz/string_algo.h"  // string_trim_copy, string_any_to_unix_copy
#include "hz/fs_path.h"  // FsPath
#include "hz/fs_path_utils.h"  // hz::filename_make_safe
#include "hz/format_unit.h"  // hz::format_date

#include "app_pcrecpp.h"
#include "storage_device.h"
#include "smartctl_parser.h"
#include "storage_settings.h"
#include "smartctl_executor.h"



// Fetch and parse basic info
// note: this will clear the non-basic properties!
std::string StorageDevice::fetch_basic_data_and_parse(hz::intrusive_ptr<CmdexSync> smartctl_ex)
{
	if (this->test_is_active_)
		return "A test is currently being performed on this drive.";

	this->clear_fetched();  // clear everything fetched before, including outputs

	std::string output;
	// We don't use "--all" - it may cause really screwed up the output (tests, etc...).
	// This looks just like "--info" only on non-smart devices.
	// --info --health --capabilities
	std::string error_msg = execute_smartctl("-i -H -c", smartctl_ex, output, true);  // set type to invalid if needed

	// Smartctl 5.39 cvs/svn version defaults to usb type on at least linux and windows.
	// This means that the old SCSI identify command isn't executed by default,
	// and there is no information about the device manufacturer/etc... in the output.
	// We detect this and set the device type to scsi to at least have _some_ info.
	if (get_type() == type_invalid) {
		debug_out_info("app", "The device seems to be of different type than auto-detected, trying again with scsi.\n");
		this->set_type(type_scsi);
		return this->fetch_basic_data_and_parse(smartctl_ex);  // try again with scsi
	}

	// Since the type error leads to "command line didn't parse" error here,
	// we do this after the scsi stuff.
	if (!error_msg.empty())
		return error_msg;

	this->info_output_ = output;

	// Set some properties too - they are needed for e.g. AODC status, etc...
	return this->parse_basic_data(true);
}




// note: this will clear the non-basic properties!
std::string StorageDevice::parse_basic_data(bool do_set_properties, bool emit_signal)
{
	this->clear_fetched(false);  // clear everything fetched before, except outputs

	if (this->info_output_.empty()) {
		debug_out_error("app", DBG_FUNC_MSG << "String to parse is empty.\n");
		return "Cannot read information from an empty string.";
	}

	std::string version, version_full;
	if (!SmartctlParser::parse_version(this->info_output_, version, version_full))  // is this smartctl data at all?
		return "Cannot get smartctl version information.";

	// detect type. note: we can't distinguish between sata and scsi (on linux, for -d ata switch).
	if (app_pcre_match("/this device: CD\\/DVD/mi", info_output_)) {
		this->set_type(type_cddvd);

// 	} else {
// 		std::string dev_base = get_device_base();
// 		if (!dev_base.empty() && dev_base[0] == 'h') {  // e.g. hda
// 			this->set_type(type_pata);
// 		}
	}

	// Note: We don't use SmartctlParser here, because this information
	// may be in some other format. If this information is valid, only then it's
	// passed to SmartctlParser.
	// Compared to SmartctlParser, this one is much more loose.

	// Don't put complete messages here - they change across smartctl versions.
	if (app_pcre_match("/^SMART support is:[ \\t]*Unavailable/mi", info_output_)  // cdroms output this
			|| app_pcre_match("/Device does not support SMART/mi", info_output_)  // usb flash drives, non-smart hds
			|| app_pcre_match("/Device Read Identity Failed \\(not an ATA\\/ATAPI device\\)/mi", info_output_)) {  // solaris scsi, unsupported by smartctl
		smart_supported_ = false;
		smart_enabled_ = false;

	} else if (app_pcre_match("/^SMART support is:[ \\t]*Available/mi", info_output_)
			|| app_pcre_match("/^SMART support is:[ \\t]*Ambiguous/mi", info_output_)) {
		smart_supported_ = true;

		if (app_pcre_match("/^SMART support is:[ \\t]*Enabled/mi", info_output_)) {
			smart_enabled_ = true;
		} else if (app_pcre_match("/^SMART support is:[ \\t]*Disabled/mi", info_output_)) {
			smart_enabled_ = false;
		}
	}


	std::string model;
	if (app_pcre_match("/^Device Model:[ \\t]*(.*)$/mi", info_output_, &model)) {  // HD's and cdroms
		model_name_ = hz::string_remove_adjacent_duplicates_copy(hz::string_trim_copy(model), ' ');

	} else if (app_pcre_match("/^Device:[ \\t]*(.*)$/mi", info_output_, &model)) {  // usb flash drives
		model_name_ = hz::string_remove_adjacent_duplicates_copy(hz::string_trim_copy(model), ' ');
	}


	std::string family;  // this is from smartctl's database I think
	if (app_pcre_match("/^Model Family:[ \\t]*(.*)$/mi", info_output_, &family)) {
		family_name_ = hz::string_remove_adjacent_duplicates_copy(hz::string_trim_copy(family), ' ');
	}

	std::string serial;
	if (app_pcre_match("/^Serial Number:[ \\t]*(.*)$/mi", info_output_, &serial)) {
		serial_number_ = hz::string_remove_adjacent_duplicates_copy(hz::string_trim_copy(serial), ' ');
	}


	// Note: this property is present since 5.33.
	std::string size;
	if (app_pcre_match("/^User Capacity:[ \\t]*(.*)$/mi", info_output_, &size)) {
		uint64_t bytes = 0;
		size_ = SmartctlParser::parse_byte_size(size, bytes, false);
	}


	// Try to parse the properties. ignore its errors - we already got what we came for.
	// Note that this may try to parse data the second time (it may already have
	// been parsed by parse_data() which failed at it).
	if (do_set_properties) {
		SmartctlParser ps;
		if (ps.parse_full(this->info_output_)) {  // try to parse it
			this->set_properties(ps.get_properties());  // copy to our drive, overwriting old data
		}
	}

	if (emit_signal)
		signal_changed.emit(this);  // notify listeners

	return std::string();
}



// fetch complete data (basic too), parse it.
std::string StorageDevice::fetch_data_and_parse(hz::intrusive_ptr<CmdexSync> smartctl_ex)
{
	if (this->test_is_active_)
		return "A test is currently being performed on this drive.";

	this->clear_fetched();  // clear everything fetched before, including outputs

	std::string output;
	std::string error_msg;

	// instead of -a, we use all the individual options -a encompasses, so that
	// an addition to default -a output won't affect us.
	if (this->get_type() == type_scsi) {
		// This doesn't do much yet, but just in case...
		// SCSI equivalent of -a:
		error_msg = execute_smartctl("-H -i -A -l error -l selftest", smartctl_ex, output);
	} else {
		// ATA equivalent of -a:
		error_msg = execute_smartctl("-H -i -c -A -l error -l selftest -l selective",
				smartctl_ex, output, true);  // set type to invalid if needed
	}
	// See notes above (in fetch_basic_data_and_parse()).
	if (get_type() == type_invalid) {
		debug_out_info("app", "The device seems to be of different type than auto-detected, trying again with scsi.\n");
		this->set_type(type_scsi);
		return this->fetch_data_and_parse(smartctl_ex);  // try again with scsi
	}

	// Since the type error leads to "command line didn't parse" error here,
	// we do this after the scsi stuff.
	if (!error_msg.empty())
		return error_msg;

	this->full_output_ = output;
	return this->parse_data();
}




// parses full info. if failed, tries to parse it as basic info.
// returns an error message on error.
std::string StorageDevice::parse_data()
{
	this->clear_fetched(false);  // clear everything fetched before, except outputs

	SmartctlParser ps;
	if (ps.parse_full(this->full_output_)) {  // try to parse it (parse only, set the properties after basic parsing).

		// refresh basic info too
		this->info_output_ = ps.get_data_full();  // put data including version information

		// note: this will clear the non-basic properties!
		// this will parse some info that is already parsed by SmartctlParser::parse_full(),
		// but this one sets the StorageDevice class members, not properties.
		this->parse_basic_data(false, false);  // don't emit signal, we're not complete yet.

		// set the full properties
		this->set_fully_parsed(true);
		this->set_properties(ps.get_properties());  // copy to our drive, overwriting old data

		signal_changed.emit(this);  // notify listeners

		return "";
	}

	// Don't show any GUI warnings on parse failure - it may just be an unsupported
	// drive (e.g. usb flash disk). Plus, it may flood the string. The data will be
	// parsed again in Info window, and we show the warnings there.
	debug_out_warn("app", DBG_FUNC_MSG << "Cannot parse smartctl output.\n");

	this->set_fully_parsed(false);

	// proper parsing failed. try to at least extract info section
	this->info_output_ = this->full_output_;  // complete output here. sometimes it's only the info section
	if (!this->parse_basic_data(true).empty()) {  // will add some properties too. this will emit signal_changed.
		return ps.get_error_msg();  // return full parser's error messages - they are more detailed.
	}

	return "";  // return ok if at least the info was ok.
}



// returns error message on error, empty string on success
std::string StorageDevice::set_smart_enabled(bool b, hz::intrusive_ptr<CmdexSync> smartctl_ex)
{
	if (this->test_is_active_)
		return "A test is currently being performed on this drive.";

	// execute smartctl --smart=on|off /dev/...
	// --saveauto=on is also executed when enabling smart.

	// Output:
/*
=== START OF ENABLE/DISABLE COMMANDS SECTION ===
SMART Enabled.
SMART Attribute Autosave Enabled.
--------------------------- OR ---------------------------
=== START OF ENABLE/DISABLE COMMANDS SECTION ===
SMART Disabled. Use option -s with argument 'on' to enable it.
--------------------------- OR ---------------------------
A mandatory SMART command failed: exiting. To continue, add one or more '-T permissive' options.
*/

	std::string output;
	// --smart=on --saveauto=on, --smart=off
	std::string error_msg = execute_smartctl((b ? "-s on -S on" : "-s off"), smartctl_ex, output);
	if (!error_msg.empty())
		return error_msg;

	// search at line start, because they are sometimes present in other sentences too.
	if (app_pcre_match("/^SMART Enabled/mi", output) || app_pcre_match("/^SMART Disabled/mi", output)) {
		return std::string();  // success

	} else if (app_pcre_match("/^A mandatory SMART command failed/mi", output)) {
		return "Mandatory SMART command failed.";
	}

	return "Unknown error occurred.";
}



// returns error message on error, empty string on success
std::string StorageDevice::set_aodc_enabled(bool b, hz::intrusive_ptr<CmdexSync> smartctl_ex)
{
	if (this->test_is_active_)
		return "A test is currently being performed on this drive.";

	// execute smartctl --offlineauto=on|off /dev/...
	// Output:
/*
=== START OF ENABLE/DISABLE COMMANDS SECTION ===
SMART Automatic Offline Testing Enabled every four hours.
--------------------------- OR ---------------------------
=== START OF ENABLE/DISABLE COMMANDS SECTION ===
SMART Automatic Offline Testing Disabled.
--------------------------- OR ---------------------------
A mandatory SMART command failed: exiting. To continue, add one or more '-T permissive' options.
*/
	std::string output;
	// --offlineauto=on, --offlineauto=off
	std::string error_msg = execute_smartctl((b ? "-o on" : "-o off"), smartctl_ex, output);
	if (!error_msg.empty())
		return error_msg;

	if (app_pcre_match("/Testing Enabled/mi", output) || app_pcre_match("/Testing Disabled/mi", output)) {
		return std::string();  // success

	} else if (app_pcre_match("/^A mandatory SMART command failed/mi", output)) {
		return "Mandatory SMART command failed.";
	}

	return "Unknown error occurred.";
}




// get only the filename portion
std::string StorageDevice::get_virtual_filename() const
{
	return (is_virtual_ ? hz::FsPath(virtual_file_).get_basename() : std::string());
}




StorageDevice::status_t StorageDevice::get_smart_status() const
{
	status_t status = status_unsupported;
	if (smart_enabled_.defined()) {
		if (smart_enabled_.value()) {  // enabled, supported
			status = status_enabled;
		} else {  // if it's disabled, maybe it's unsupported, check that:
			if (smart_supported_.defined()) {
				if (smart_supported_.value()) {  // disabled, supported
					status = status_disabled;
				} else {  // disabled, unsupported
					status = status_unsupported;
				}
			} else {  // disabled, support unknown
				status = status_disabled;
			}
		}
	} else {  // status unknown
		if (smart_supported_.defined()) {
			if (smart_supported_.value()) {  // status unknown, supported
				status = status_disabled;  // at least give the user a chance to try enabling it
			} else {  // status unknown, unsupported
				status = status_unsupported;  // most likely
			}
		} else {  // status unknown, support unknown
			status = status_unsupported;
		}
	}
	return status;
}



StorageDevice::status_t StorageDevice::get_aodc_status() const
{
	// smart-disabled drives are known to print some garbage, so
	// let's protect us from it.
	if (get_smart_status() != status_enabled)
		return status_unsupported;

	if (aodc_status_.defined())  // cached return value
		return aodc_status_.value();

	status_t status = status_unknown;  // for now

	bool aodc_supported = false;
	int found = 0;

	for (SmartctlParser::prop_list_t::const_iterator iter = properties_.begin(); iter != properties_.end(); ++iter) {
		if (iter->section == StorageProperty::section_internal) {
			if (iter->generic_name == "aodc_enabled") {  // if this is not present at all, we set the unknown status.
				status = (iter->value_bool ? status_enabled : status_disabled);
				//++found;
				continue;
			}
			if (iter->generic_name == "aodc_support") {
				aodc_supported = iter->value_bool;
				++found;
				continue;
			}
			if (found >= 2)
				break;
		}
	}

	if (!aodc_supported)
		status = status_unsupported;
	// if it's supported, then status may be enabled, disabled or unknown.

	aodc_status_ = status;  // store to cache

	debug_out_info("app", DBG_FUNC_MSG << "AODC status: " << get_status_name(status) << "\n");

	return status;
}



// returns empty string on error, format size string on success.
std::string StorageDevice::get_device_size_str() const
{
	return (size_.defined() ? size_.value() : "");
}



StorageProperty StorageDevice::get_health_property() const
{
	if (health_property_.defined())  // cached return value
		return health_property_.value();

	StorageProperty p = this->lookup_property("overall_health",
			StorageProperty::section_data, StorageProperty::subsection_health);
	if (!p.empty())
		health_property_ = p;  // store to cache

	return p;
}



std::string StorageDevice::get_save_filename() const
{
	std::string model = this->get_model_name();  // may be empty
	std::string serial = this->get_serial_number();
	std::string date = hz::format_date("%Y-%m-%d", false);

	std::string filename_format;
	rconfig::get_data("gui/smartctl_output_filename_format", filename_format);
	hz::string_replace(filename_format, "{serial}", serial);
	hz::string_replace(filename_format, "{model}", model);
	hz::string_replace(filename_format, "{date}", date);

	return hz::filename_make_safe(filename_format);
}




std::string StorageDevice::get_device_options() const
{
	if (is_virtual_) {
		debug_out_warn("app", DBG_FUNC_MSG << "Cannot get device options of a virtual device.\n");
		return std::string();
	}

	std::string config_options = app_get_device_option(get_device());

	// If we have some special type, specify it on the command line (like "-d scsi").
	// Note that the latter "-d" option overrides the former, so we're ok with multiple ones.
	std::string type_arg = get_type_arg_name(this->get_type());

	if (!type_arg.empty()) {
 	if (!config_options.empty()) {
			config_options += " ";
		}
		config_options += "-d " + type_arg;
	}

	return config_options;
}



// Returns error message on error, empty string on success
std::string StorageDevice::execute_smartctl(const std::string& command_options,
		hz::intrusive_ptr<CmdexSync> smartctl_ex, std::string& smartctl_output, bool check_type)
{
	// don't forbid running on currently tested drive - we need to call this from the test code.

	if (is_virtual_) {
		debug_out_warn("app", DBG_FUNC_MSG << "Cannot execute smartctl on a virtual device.\n");
		return "Cannot execute smartctl on a virtual device.";
	}

	std::string device = get_device();

#ifndef _WIN32  // win32 doesn't have slashes in devices names
	{
		std::string::size_type pos = device.rfind('/');  // find basename
		if (pos == std::string::npos) {
			debug_out_error("app", DBG_FUNC_MSG << "Invalid device name \"" << device << "\".\n");
			return "Invalid device name specified.";
		}
	}
#endif

	if (!smartctl_ex)  // if it doesn't exist, create a default one
		smartctl_ex = new SmartctlExecutor();  // will be auto-deleted

	std::string smartctl_binary = get_smartctl_binary();

	if (smartctl_binary.empty()) {
		debug_out_error("app", DBG_FUNC_MSG << "Smartctl binary is not set in config.\n");
		return "Smartctl binary is not specified in configuration.";
	}

	std::string smartctl_def_options;
	rconfig::get_data("system/smartctl_options", smartctl_def_options);

	if (!smartctl_def_options.empty())
		smartctl_def_options += " ";


	std::string device_specific_options = this->get_device_options();
	if (!device_specific_options.empty())
		device_specific_options += " ";


	smartctl_ex->set_command(Glib::shell_quote(smartctl_binary),
			smartctl_def_options + device_specific_options + command_options + " " + Glib::shell_quote(device));

	if (!smartctl_ex->execute() || !smartctl_ex->get_error_msg().empty()) {
		debug_out_warn("app", DBG_FUNC_MSG << "Error while executing smartctl binary.\n");

		std::string output = smartctl_ex->get_stdout_str();

		// check if it's a device permission error.
		// Smartctl open device: /dev/sdb failed: Permission denied
		if (app_pcre_match("/Smartctl open device.+Permission denied/mi", output)) {
			return "Permission denied while opening device.";
		}

		// Smartctl 5.39 cvs/svn version defaults to usb type on at least linux and windows.
		// This means that the old SCSI identify command isn't executed by default,
		// and there is no information about the device manufacturer/etc... in the output.
		// We detect this and set the device type to scsi to at least have _some_ info.
		if (check_type && this->get_type() == type_unknown
				&& app_pcre_match("/specify device type with the -d option/mi", output)) {
			this->set_type(type_invalid);
		}

		return smartctl_ex->get_error_msg();
	}

	// any_to_unix is needed for windows
	smartctl_output = hz::string_trim_copy(hz::string_any_to_unix_copy(smartctl_ex->get_stdout_str()));
	if (smartctl_output.empty()) {
		debug_out_error("app", DBG_FUNC_MSG << "Smartctl returned an empty output.\n");
		return "Smartctl returned an empty output.";
	}

	return std::string();
}







