/******************************************************************************
License: GNU General Public License v3.0 only
Copyright:
	(C) 2008 - 2021 Alexander Shaduri <ashaduri@gmail.com>
******************************************************************************/
/// \file
/// \author Alexander Shaduri
/// \ingroup applib
/// \weakgroup applib
/// @{

#include "storage_device.h"

#include <glibmm.h>
#include <cctype>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "rconfig/rconfig.h"
#include "hz/string_algo.h"  // string_trim_copy, string_any_to_unix_copy
#include "hz/fs.h"
#include "hz/format_unit.h"  // hz::format_date
#include "hz/error_container.h"

#include "app_regex.h"
#include "smartctl_parser_types.h"
#include "storage_device_detected_type.h"
#include "storage_settings.h"
#include "smartctl_executor.h"
#include "smartctl_version_parser.h"
#include "storage_property_descr.h"
#include "build_config.h"
//#include "smartctl_text_parser_helper.h"
//#include "ata_storage_property_descr.h"



std::string StorageDevice::get_status_displayable_name(SmartStatus status)
{
	static const std::unordered_map<SmartStatus, std::string> m {
			{SmartStatus::Enabled,     C_("status", "Enabled")},
			{SmartStatus::Disabled,    C_("status", "Disabled")},
			{SmartStatus::Unsupported, C_("status", "Unsupported")},
	};
	if (auto iter = m.find(status); iter != m.end()) {
		return iter->second;
	}
	return "[internal_error]";
}



StorageDevice::StorageDevice(std::string dev_or_vfile, bool is_virtual)
		: is_virtual_(is_virtual)
{
	if (is_virtual_) {
		virtual_file_ = hz::fs_path_from_string(dev_or_vfile);
	} else {
		device_ = std::move(dev_or_vfile);
	}
}



StorageDevice::StorageDevice(std::string dev, std::string type_arg)
		: device_(std::move(dev)), type_arg_(std::move(type_arg))
{ }



void StorageDevice::clear_outputs()
{
	basic_output_.clear();
	full_output_.clear();
}



void StorageDevice::clear_parse_results()
{
	parse_status_ = ParseStatus::None;
//	test_is_active_ = false;  // not sure

	property_repository_.clear();

	smart_supported_.reset();
	smart_enabled_.reset();
	model_name_.reset();
	family_name_.reset();
	size_.reset();
	health_property_.reset();
}



hz::ExpectedVoid<StorageDeviceError> StorageDevice::fetch_basic_data_and_parse(
		const std::shared_ptr<CommandExecutor>& smartctl_ex)
{
	if (this->test_is_active_) {
		return hz::Unexpected(StorageDeviceError::TestRunning, _("A test is currently being performed on this drive."));
	}

	// Clear everything fetched before, including outputs
	this->clear_parse_results();
	this->clear_outputs();

	// We don't use "--all" - it may cause really screwed up the output (tests, etc.).
	// This looks just like "--info" only on non-smart devices.
	const auto default_parser_type = SmartctlVersionParser::get_default_format(SmartctlParserType::Basic);
	std::vector<std::string> command_options = {"--info", "--health", "--capabilities"};
	if (default_parser_type == SmartctlOutputFormat::Json) {
		// --json flags: o means include original output (just in case).
		command_options.push_back("--json=o");
	}

	auto execute_status = execute_device_smartctl(command_options, smartctl_ex, this->basic_output_, true);  // set type to invalid if needed

	// Smartctl 5.39 cvs/svn version defaults to usb type on at least linux and windows.
	// This means that the old SCSI identify command isn't executed by default,
	// and there is no information about the device manufacturer/etc. in the output.
	// We detect this and set the device type to scsi to at least have _some_ info.

	// Note: This match works even with JSON (the text output is included in --json=o).
	if ((execute_status || execute_status.error().data() == StorageDeviceError::ExecutionError)
			&& get_detected_type() == StorageDeviceDetectedType::NeedsExplicitType
			&& get_type_argument().empty() ) {
		debug_out_info("app", "The device seems to be of different type than auto-detected, trying again with scsi.\n");
		this->set_type_argument("scsi");
		this->set_detected_type(StorageDeviceDetectedType::BasicScsi);
		return this->fetch_basic_data_and_parse(smartctl_ex);  // try again with scsi
	}

	// Since the type error leads to "command line didn't parse" error here,
	// we do this after the scsi stuff.
//	if (!execute_status.has_value()) {
		// Still try to parse something. For some reason, running smartctl on usb flash drive
		// under winxp returns "command line didn't parse", while actually printing its name.
//		this->parse_basic_data(false, true);
//		return execute_status;
//	}

	// Set some properties too - they are needed for e.g. SMART on/off support, etc.
	return this->parse_basic_data();
}



hz::ExpectedVoid<StorageDeviceError> StorageDevice::parse_basic_data()
{
	// Clear everything fetched before, except outputs and type
	this->clear_parse_results();

	// Detect the output format.
	auto detect_status = SmartctlParser::detect_output_format(this->get_basic_output());
	auto output_format = SmartctlOutputFormat::Text;
	if (detect_status.has_value()) {
		output_format = detect_status.value();
	} else {
		debug_out_warn("app", "Cannot detect smartctl output format. Assuming Text.\n");
	}

	// Parse using Basic parser. This supports all drive types.
	auto basic_parser = SmartctlParser::create(SmartctlParserType::Basic, output_format);
	DBG_ASSERT_RETURN(basic_parser, hz::Unexpected(StorageDeviceError::ParseError, _("Cannot create parser")));

	// This also fills the drive type properties.
	auto parse_status = basic_parser->parse(this->get_basic_output());
	if (!parse_status) {
		std::string message = parse_status.error().message();
		return hz::Unexpected(StorageDeviceError::ParseError,
				fmt::format(fmt::runtime(_("Cannot parse smartctl output: {}")), message));
	}

	// See if we can narrow down the drive type from what was detected
	// by StorageDetector and properties set by Basic parser.
	auto basic_property_repo = basic_parser->get_property_repository();

	// Make detected type more exact.
	detect_drive_type_from_properties(basic_property_repo);

	// Add property descriptions and set to the drive.
	this->set_property_repository(StoragePropertyProcessor::process_properties(basic_property_repo, get_detected_type()));

	debug_out_dump("app", "Drive " << get_device_with_type() << " set to be "
			<< StorageDeviceDetectedTypeExt::get_displayable_name(get_detected_type()) << " device.\n");

	read_common_properties();  // sets model_name_, etc.

	// A model field (and its aliases) is a good indication whether there was any data or not
	set_parse_status(model_name_.has_value() ? ParseStatus::Basic : ParseStatus::None);

	// Try to parse the properties. ignore its errors - we already got what we came for.
	// Note that this may try to parse data the second time (it may already have
	// been parsed by parse_data() which failed at it).
//	if (do_set_properties) {
//		auto parser = SmartctlParser::create(SmartctlParserType::Ata, output_format);
//		DBG_ASSERT_RETURN(parser, hz::Unexpected(StorageDeviceError::ParseError, _("Cannot create parser")));
//
//		if (parser->parse(this->basic_output_)) {  // try to parse it
//			this->set_property_repository(
//					StoragePropertyProcessor::process_properties(parser->get_property_repository(), disk_type));  // copy to our drive, overwriting old data
//		}
//	}

	signal_changed().emit(this);  // notify listeners

	return {};
}



hz::ExpectedVoid<StorageDeviceError> StorageDevice::fetch_full_data_and_parse(
		const std::shared_ptr<CommandExecutor>& smartctl_ex)
{
	if (this->test_is_active_) {
		return hz::Unexpected(StorageDeviceError::TestRunning, _("A test is currently being performed on this drive."));
	}

	// Drive type must be already set at this point, using fetch_basic_data_and_parse().
	DBG_ASSERT(this->get_detected_type() != StorageDeviceDetectedType::Unknown);

	// Clear everything fetched before, including outputs
	this->clear_parse_results();
	this->clear_outputs();

	// Execute smartctl.

	// Instead of -x, we use all the individual options -x encompasses, so that
	// an addition to default -x output won't affect us.
	std::vector<std::string> command_options;

	// Type was detected by Basic parser
	switch (this->get_detected_type()) {
		case StorageDeviceDetectedType::Unknown:
		case StorageDeviceDetectedType::NeedsExplicitType:
			DBG_ASSERT(0);
			break;
		case StorageDeviceDetectedType::AtaAny:
		case StorageDeviceDetectedType::AtaHdd:
		case StorageDeviceDetectedType::AtaSsd:
			command_options = {
					"--health",
					"--info",
					"--get=all",
					"--capabilities",
					"--attributes",
					"--format=brief",
					"--log=xerror,50,error",
					"--log=xselftest,50,selftest",
					"--log=selective",
					"--log=directory",
					"--log=scttemp",
					"--log=scterc",
					"--log=devstat",
					"--log=sataphy",
			};
			break;
		case StorageDeviceDetectedType::Nvme:
			// We don't care if something is added to json output.
			// Same as: --health --info --capabilities --attributes --log=error --log=selftest
			command_options = {"--xall"};
			break;
		case StorageDeviceDetectedType::BasicScsi:
		case StorageDeviceDetectedType::CdDvd:
		case StorageDeviceDetectedType::UnsupportedRaid:
			// SCSI equivalent of -x:
			// command_options = "--health --info --attributes --log=error --log=selftest --log=background --log=sasphy";
			command_options = {"--xall"};
			break;
	}

	auto parser_type = SmartctlVersionParser::get_default_parser_type(this->get_detected_type());
	auto parser_format = SmartctlVersionParser::get_default_format(parser_type);
	if (parser_format == SmartctlOutputFormat::Json) {
		// --json flags: o means include original output (just in case).
		command_options.push_back("--json=o");
	}

	std::string output;
	auto execute_status = execute_device_smartctl(command_options, smartctl_ex, output);

//	if (this->get_type_argument() == "scsi") {  // not sure about correctness... FIXME probably fails with RAID/scsi
//		const auto default_parser_type = SmartctlVersionParser::get_default_format(SmartctlParserType::Basic);
//		// This doesn't do much yet, but just in case...
//		// SCSI equivalent of -x:
//		std::string command_options = "--health --info --attributes --log=error --log=selftest --log=background --log=sasphy";
//		if (default_parser_type == SmartctlOutputFormat::Json) {
//			// --json flags: o means include original output (just in case).
//			command_options += " --json=o";
//		}
//
//		execute_status = execute_device_smartctl(command_options, smartctl_ex, output);
//
//	} else {
//		const auto default_parser_type = SmartctlVersionParser::get_default_format(SmartctlParserType::Ata);
//		// ATA equivalent of -x.
//		std::string command_options = "--health --info --get=all --capabilities --attributes --format=brief --log=xerror,50,error --log=xselftest,50,selftest --log=selective --log=directory --log=scttemp --log=scterc --log=devstat --log=sataphy";
//		if (default_parser_type == SmartctlOutputFormat::Json) {
//			// --json flags: o means include original output (just in case).
//			command_options += " --json=o";
//		}
//
//		execute_status = execute_device_smartctl(command_options, smartctl_ex, output, true);  // set type to invalid if needed
//	}
	// See notes above (in fetch_basic_data_and_parse()).
	// No need to do this: if the basic data was fetched, the type is already set.
//	if ((execute_status || execute_status.error().data() == StorageDeviceError::ExecutionError)
//			&& get_detected_type() == DetectedType::NeedsExplicitType && get_type_argument().empty()) {
//		debug_out_info("app", "The device seems to be of different type than auto-detected, trying again with scsi.\n");
//		this->set_type_argument("scsi");
//		return this->fetch_full_data_and_parse(smartctl_ex);  // try again with scsi
//	}
	// Since the type error leads to "command line didn't parse" error here,
	// we do this after the scsi stuff.


	if (!execute_status)
		return execute_status;

	this->full_output_ = output;
	return this->parse_full_data(parser_type, parser_format);
}


/*
hz::ExpectedVoid<StorageDeviceError> StorageDevice::try_parse_data()
{
	this->clear_fetched(false);  // clear everything fetched before, except outputs and disk type

	auto parser_format = SmartctlParser::detect_output_format(this->full_output_);
	if (!parser_format.has_value()) {
		return hz::Unexpected(StorageDeviceError::ParseError, parser_format.error().message());
	}

//	auto basic_parser = SmartctlParser::create(SmartctlParserType::Basic, parser_format.value());
//	if (!basic_parser) {
//		return hz::Unexpected(StorageDeviceError::ParseError, _("Cannot create parser"));
//	}

	// TODO Choose format according to device type
	SmartctlParserType parser_type = SmartctlParserType::Ata;

	auto parser = SmartctlParser::create(parser_type, parser_format.value());
	DBG_ASSERT_RETURN(parser, hz::Unexpected(StorageDeviceError::ParseError, _("Cannot create parser")));

	// Try to parse it (parse only, set the properties after basic parsing).
	const auto parse_status = parser->parse(this->full_output_);
	if (parse_status.has_value()) {

		// refresh basic info too
		this->basic_output_ = this->full_output_;  // put data including version information

		// note: this will clear the non-basic properties!
		// this will parse some info that is already parsed by SmartctlAtaTextParser::parse(),
		// but this one sets the StorageDevice class members, not properties.
		static_cast<void>(this->parse_basic_data(false, false));  // don't emit signal, we're not complete yet.

		// Call this after parse_basic_data(), since it sets parse status to "info".
		this->set_parse_status(StorageDevice::ParseStatus::Full);

		// set the full properties.
		// copy to our drive, overwriting old data.
		this->set_property_repository(StoragePropertyProcessor::process_properties(parser->get_property_repository(), disk_type));

		signal_changed().emit(this);  // notify listeners

		return {};
	}

	// Don't show any GUI warnings on parse failure - it may just be an unsupported
	// drive (e.g. usb flash disk). Plus, it may flood the string. The data will be
	// parsed again in Info window, and we show the warnings there.
	debug_out_warn("app", DBG_FUNC_MSG << "Cannot parse smartctl output.\n");

	// proper parsing failed. try to at least extract info section
	this->basic_output_ = this->full_output_;  // complete output here. sometimes it's only the info section
	auto basic_parse_status = this->parse_basic_data(true);  // will add some properties too. this will emit signal_changed().
	if (!basic_parse_status) {
		// return full parser's error messages - they are more detailed.
		std::string message = basic_parse_status.error().message();
		return hz::Unexpected(StorageDeviceError::ParseError,
				fmt::format(fmt::runtime(_("Cannot parse smartctl output: {}")), message));
	}

	return {};  // return ok if at least the info was ok.
}
*/



hz::ExpectedVoid<StorageDeviceError> StorageDevice::parse_full_data(SmartctlParserType parser_type, SmartctlOutputFormat format)
{
	// Clear everything fetched before, except outputs and disk type
	clear_parse_results();

	auto parser = SmartctlParser::create(parser_type, format);
	DBG_ASSERT_RETURN(parser, hz::Unexpected(StorageDeviceError::ParseError, _("Cannot create parser")));

	const auto parse_status = parser->parse(this->full_output_);
	if (parse_status.has_value()) {
		set_parse_status(parser_type == SmartctlParserType::Basic ? ParseStatus::Basic : ParseStatus::Full);

		// Detect drive type based on parsed properties
		detect_drive_type_from_properties(parser->get_property_repository());

		// Set the full properties, overwriting old data.
		set_property_repository(StoragePropertyProcessor::process_properties(parser->get_property_repository(), get_detected_type()));

		// Read common properties from the repository.
		read_common_properties();

		signal_changed().emit(this);  // notify listeners

		return {};
	}

	std::string message = parse_status.error().message();
	return hz::Unexpected(StorageDeviceError::ParseError,
			fmt::format(fmt::runtime(_("Cannot parse smartctl output: {}")), message));
}



hz::ExpectedVoid<StorageDeviceError> StorageDevice::parse_any_data_for_virtual()
{
	// Clear everything fetched before, except outputs and disk type
	this->clear_parse_results();

	auto parser_format = SmartctlParser::detect_output_format(this->full_output_);
	if (!parser_format.has_value()) {
		return hz::Unexpected(StorageDeviceError::ParseError, parser_format.error().message());
	}

	auto basic_parser = SmartctlParser::create(SmartctlParserType::Basic, parser_format.value());
	if (!basic_parser) {
		return hz::Unexpected(StorageDeviceError::ParseError, _("Cannot create parser"));
	}

	// This will add some properties and emit signal_changed().
	auto basic_parse_status = basic_parser->parse(this->full_output_);
	if (!basic_parse_status) {
		std::string message = basic_parse_status.error().message();
		return hz::Unexpected(StorageDeviceError::ParseError,
				fmt::format(fmt::runtime(_("Cannot parse smartctl output: {}")), message));
	}

	auto basic_property_repo = basic_parser->get_property_repository();

	// Make detected type more exact.
	detect_drive_type_from_properties(basic_property_repo);

	// Set properties from the basic parser.
	set_property_repository(StoragePropertyProcessor::process_properties(basic_property_repo, get_detected_type()));

	// Read common properties from the repository.
	read_common_properties();


	// Try to parse with a specialized parser based on drive type
	auto parser_type = SmartctlVersionParser::get_default_parser_type(this->get_detected_type());

	if (parser_type != SmartctlParserType::Basic) {
		// Try specialized parser
		auto parser = SmartctlParser::create(parser_type, parser_format.value());
		DBG_ASSERT_RETURN(parser, hz::Unexpected(StorageDeviceError::ParseError, _("Cannot create parser.")));

		const auto parse_status = parser->parse(this->full_output_);
		if (parse_status.has_value()) {
			// Call this after parse_basic_data(), since it sets parse status to "info".
			set_parse_status(StorageDevice::ParseStatus::Full);

			// set the full properties.
			// copy to our drive, overwriting old data.
			set_property_repository(StoragePropertyProcessor::process_properties(parser->get_property_repository(), get_detected_type()));
		}
	}

	if (get_parse_status() != ParseStatus::Full) {
		// Only basic data available
		set_parse_status(ParseStatus::Basic);
		set_property_repository(StoragePropertyProcessor::process_properties(basic_parser->get_property_repository(), get_detected_type()));
	}

	signal_changed().emit(this);  // notify listeners

	// Don't show any GUI warnings on parse failure - it may just be an unsupported
	// drive (e.g. usb flash disk). Plus, it may flood the string. The data will be
	// parsed again in Info window, and we show the warnings there.
//	debug_out_warn("app", DBG_FUNC_MSG << "Cannot parse smartctl output.\n");

	// proper parsing failed. try to at least extract info section
//	this->basic_output_ = this->full_output_;  // complete output here. sometimes it's only the info section

	return {};
}



StorageDevice::ParseStatus StorageDevice::get_parse_status() const
{
	return parse_status_;
}



hz::ExpectedVoid<StorageDeviceError> StorageDevice::set_smart_enabled(bool b,
		const std::shared_ptr<CommandExecutor>& smartctl_ex)
{
	if (this->test_is_active_) {
		return hz::Unexpected(StorageDeviceError::TestRunning, _("A test is currently being performed on this drive."));
	}

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
	std::vector<std::string> on_command = {"--smart=on", "--saveauto=on"};
	std::vector<std::string> off_command = {"--smart=off"};
	auto status = execute_device_smartctl((b ? on_command : off_command), smartctl_ex, output);
	if (!status) {
		return status;
	}

	// search at line start, because they are sometimes present in other sentences too.
	if (app_regex_partial_match("/^SMART Enabled/mi", output)
			|| app_regex_partial_match("/^SMART Disabled/mi", output)) {
		return {};  // success
	}

	if (app_regex_partial_match("/^A mandatory SMART command failed/mi", output)) {
		return hz::Unexpected(StorageDeviceError::CommandFailed, _("Mandatory SMART command failed."));
	}

	return hz::Unexpected(StorageDeviceError::CommandUnknownError, _("Unknown error occurred."));
}



void StorageDevice::read_common_properties()
{
	if (auto prop = property_repository_.lookup_property("smart_support/available"); !prop.empty()) {
		smart_supported_ = prop.get_value<bool>();
	}
	if (auto prop = property_repository_.lookup_property("smart_support/enabled"); !prop.empty()) {
		smart_enabled_ = prop.get_value<bool>();
	}
	if (auto prop = property_repository_.lookup_property("model_name"); !prop.empty()) {
		model_name_ = prop.get_value<std::string>();
	} else if (prop = property_repository_.lookup_property("scsi_model_name"); !prop.empty()) {  // USB flash
		model_name_ = prop.get_value<std::string>();
	}
	if (auto prop = property_repository_.lookup_property("model_family"); !prop.empty()) {
		family_name_ = prop.get_value<std::string>();
	} else if (prop = property_repository_.lookup_property("scsi_vendor"); !prop.empty()) {  // USB flash
		family_name_ = prop.get_value<std::string>();
	}
	if (auto prop = property_repository_.lookup_property("serial_number"); !prop.empty()) {
		serial_number_ = prop.get_value<std::string>();
	}
	if (auto prop = property_repository_.lookup_property("user_capacity/bytes/_short"); !prop.empty()) {
		size_ = prop.readable_value;
	} else if (auto prop2 = property_repository_.lookup_property("user_capacity/bytes"); !prop.empty()) {
		size_ = prop2.readable_value;
	}
}



void StorageDevice::detect_drive_type_from_properties(const StoragePropertyRepository& property_repo)
{
	// This is set by Text parser
	if (auto drive_type_prop = property_repo.lookup_property("_text_only/custom/parser_detected_drive_type"); !drive_type_prop.empty()) {
		const auto& drive_type_storable_str = drive_type_prop.get_value<std::string>();
		set_detected_type(StorageDeviceDetectedTypeExt::get_by_storable_name(drive_type_storable_str, StorageDeviceDetectedType::BasicScsi));

		// Find out if it's SSD or HDD
		if (get_detected_type() == StorageDeviceDetectedType::AtaAny) {
			auto rpm_prop = property_repo.lookup_property("rotation_rate");
			if (rpm_prop.empty() || rpm_prop.get_value<std::int64_t>() == 0) {
				set_detected_type(StorageDeviceDetectedType::AtaSsd);
			} else {
				set_detected_type(StorageDeviceDetectedType::AtaHdd);
			}
		}
	}

	// This is set by JSON parser
	if (auto device_type_prop = property_repo.lookup_property("device/type"); !device_type_prop.empty()) {
		// Note: USB flash drives in non-scsi mode do not have this property.
		const auto& smartctl_type = device_type_prop.get_value<std::string>();

		std::string lowercase_protocol;
		if (auto device_protocol_prop = property_repo.lookup_property("device/protocol"); !device_protocol_prop.empty()) {
			lowercase_protocol = hz::string_to_lower_copy(device_protocol_prop.get_value<std::string>());
		}

		// USB flash in scsi mode, optical, scsi, etc.
		// Protocol is also "SCSI".
		if (smartctl_type == "scsi") {
			if (BuildEnv::is_kernel_linux() && get_device_base().starts_with("sr")) {
				set_detected_type(StorageDeviceDetectedType::CdDvd);
			} else {
				set_detected_type(StorageDeviceDetectedType::BasicScsi);
			}

		// (S)ATA, including behind supported RAID controllers
		} else if (smartctl_type == "sat" || lowercase_protocol == "ata") {
			// Find out if it's SSD or HDD
			auto rpm_prop = property_repo.lookup_property("rotation_rate");
			if (rpm_prop.empty() || rpm_prop.get_value<std::int64_t>() == 0) {
				set_detected_type(StorageDeviceDetectedType::AtaSsd);
			} else {
				set_detected_type(StorageDeviceDetectedType::AtaHdd);
			}

		// NVMe SSD.
		// Note: NVMe behind USB bridge may have type "sntrealtek" or similar, with protocol "nvme".
		} else if (smartctl_type == "nvme" || lowercase_protocol == "nvme") {
			set_detected_type(StorageDeviceDetectedType::Nvme);

		} else {
			// TODO Detect unsupported RAID
			debug_out_warn("app", "Unsupported type " << smartctl_type << " (protocol: " << lowercase_protocol << ") reported by smartctl for " << get_device_with_type() << "\n");
		}
	}

	if (get_detected_type() == StorageDeviceDetectedType::Unknown) {
		set_detected_type(StorageDeviceDetectedType::BasicScsi);  // fall back to basic scsi parser
	}

	debug_out_info("app", "Device " << get_device_with_type() << " detected after parser to be of type "
			<< StorageDeviceDetectedTypeExt::get_storable_name(get_detected_type()) << "\n");
}



StorageDevice::SmartStatus StorageDevice::get_smart_status() const
{
	SmartStatus status = SmartStatus::Unsupported;
	if (smart_enabled_.has_value()) {
		if (smart_enabled_.value()) {  // enabled, supported
			status = SmartStatus::Enabled;
		} else {  // if it's disabled, maybe it's unsupported, check that:
			if (smart_supported_.has_value()) {
				if (smart_supported_.value()) {  // disabled, supported
					status = SmartStatus::Disabled;
				} else {  // disabled, unsupported
					status = SmartStatus::Unsupported;
				}
			} else {  // disabled, support unknown
				status = SmartStatus::Disabled;
			}
		}
	} else {  // status unknown
		if (smart_supported_.has_value()) {
			if (smart_supported_.value()) {  // status unknown, supported
				status = SmartStatus::Disabled;  // at least give the user a chance to try enabling it
			} else {  // status unknown, unsupported
				status = SmartStatus::Unsupported;  // most likely
			}
		} else {  // status unknown, support unknown
			status = SmartStatus::Unsupported;
		}
	}
	return status;
}



bool StorageDevice::get_smart_switch_supported() const
{
	const bool supported = get_smart_status() != SmartStatus::Unsupported;
	// NVMe does not support on/off
	const bool is_nvme = get_detected_type() == StorageDeviceDetectedType::Nvme;

	return !get_is_virtual() && supported && !is_nvme;
}



std::string StorageDevice::get_device_size_str() const
{
	return (size_.has_value() ? size_.value() : "");
}



StorageProperty StorageDevice::get_health_property() const
{
	if (health_property_.has_value())  // cached return value
		return health_property_.value();

	StorageProperty p = property_repository_.lookup_property("smart_status/passed",
			StoragePropertySection::OverallHealth);
	if (!p.empty())
		health_property_ = p;  // store to cache

	return p;
}



std::string StorageDevice::get_device() const
{
	return device_;
}



std::string StorageDevice::get_device_base() const
{
	if (is_virtual_)
		return "";

	const std::string::size_type pos = device_.rfind('/');  // find basename
	if (pos == std::string::npos)
		return device_;  // fall back
	return device_.substr(pos+1, std::string::npos);
}



std::string StorageDevice::get_device_with_type() const
{
	if (this->get_is_virtual()) {
		const std::string vf = this->get_virtual_filename();
		/// Translators: %1 is filename
		std::string ret = Glib::ustring::compose(C_("filename", "Virtual (%1)"), (vf.empty() ? (std::string("[") + C_("filename", "empty") + "]") : vf));
		return ret;
	}
	std::string device = get_device();
	if (!get_type_argument().empty()) {
		device = Glib::ustring::compose(_("%1 (%2)"), device, get_type_argument());
	}
	return device;
}



void StorageDevice::set_detected_type(StorageDeviceDetectedType t)
{
	detected_type_ = t;
}



StorageDeviceDetectedType StorageDevice::get_detected_type() const
{
	return detected_type_;
}



void StorageDevice::set_type_argument(std::string arg)
{
	type_arg_ = std::move(arg);
}



std::string StorageDevice::get_type_argument() const
{
	return type_arg_;
}



void StorageDevice::set_extra_arguments(std::vector<std::string> args)
{
	extra_args_ = std::move(args);
}



std::vector<std::string> StorageDevice::get_extra_arguments() const
{
	return extra_args_;
}



void StorageDevice::set_drive_letters(std::map<char, std::string> letters)
{
	drive_letters_ = std::move(letters);
}



const std::map<char, std::string>& StorageDevice::get_drive_letters() const
{
	return drive_letters_;
}



std::string StorageDevice::format_drive_letters(bool with_volnames) const
{
	std::vector<std::string> drive_letters_decorated;
	for (const auto& iter : drive_letters_) {
		drive_letters_decorated.push_back(std::string() + (char)std::toupper(iter.first) + ":");
		if (with_volnames && !iter.second.empty()) {
			// e.g. "C: (Local Drive)"
			drive_letters_decorated.back() = Glib::ustring::compose(_("%1 (%2)"), drive_letters_decorated.back(), iter.second);
		}
	}
	return hz::string_join(drive_letters_decorated, ", ");
}



bool StorageDevice::get_is_virtual() const
{
	return is_virtual_;
}



hz::fs::path StorageDevice::get_virtual_file() const
{
	return (is_virtual_ ? virtual_file_ : hz::fs::path());
}



std::string StorageDevice::get_virtual_filename() const
{
	return (is_virtual_ ? hz::fs_path_to_string(virtual_file_.filename()) : std::string());
}



const StoragePropertyRepository& StorageDevice::get_property_repository() const
{
	return property_repository_;
}



std::string StorageDevice::get_model_name() const
{
	return (model_name_.has_value() ? model_name_.value() : "");
}



std::string StorageDevice::get_family_name() const
{
	return (family_name_.has_value() ? family_name_.value() : "");
}



std::string StorageDevice::get_serial_number() const
{
	return (serial_number_.has_value() ? serial_number_.value() : "");
}



void StorageDevice::set_info_output(std::string s)
{
	basic_output_ = std::move(s);
}



std::string StorageDevice::get_basic_output() const
{
	return basic_output_;
}



void StorageDevice::set_full_output(std::string s)
{
	full_output_ = std::move(s);
}



std::string StorageDevice::get_full_output() const
{
	return full_output_;
}



void StorageDevice::set_is_manually_added(bool b)
{
	is_manually_added_ = b;
}



bool StorageDevice::get_is_manually_added() const
{
	return is_manually_added_;
}



void StorageDevice::set_test_is_active(bool b)
{
	const bool changed = (test_is_active_ != b);
	test_is_active_ = b;
	if (changed) {
		signal_changed().emit(this);  // so that everybody stops any test-aborting operations.
	}
}



bool StorageDevice::get_test_is_active() const
{
	return test_is_active_;
}



StorageDevice::SelfTestSupportStatus StorageDevice::get_self_test_support_status() const
{
	if (get_parse_status() == ParseStatus::Full) {
		return property_repository_.has_properties_for_section(StoragePropertySection::SelftestLog) ?
				SelfTestSupportStatus::Supported : SelfTestSupportStatus::Unsupported;
	}
	if (get_parse_status() == ParseStatus::Basic) {
		return get_smart_status() == SmartStatus::Enabled ? SelfTestSupportStatus::Unknown : SelfTestSupportStatus::Unsupported;
	}
	return StorageDevice::SelfTestSupportStatus::Unknown;
}



std::string StorageDevice::get_save_filename() const
{
	const std::string model = this->get_model_name();  // may be empty
	const std::string serial = this->get_serial_number();
	const std::string date = hz::format_date("%Y-%m-%d_%H%M", true);

	auto filename_format = rconfig::get_data<std::string>("gui/smartctl_output_filename_format");
	hz::string_replace(filename_format, "{serial}", serial);
	hz::string_replace(filename_format, "{model}", model);
	hz::string_replace(filename_format, "{date}", date);

	return hz::fs_filename_make_safe(filename_format);
}



std::vector<std::string> StorageDevice::get_device_options() const
{
	if (is_virtual_) {
		debug_out_warn("app", DBG_FUNC_MSG << "Cannot get device options of a virtual device.\n");
		return {};
	}

	// If we have some special type or option, specify it on the command line (like "-d scsi").
	// Note that the latter "-d" option overrides the former.

	// lowest priority - the detected type
	std::vector<std::string> args;
	if (!get_type_argument().empty()) {
		args.emplace_back("-d");
		args.push_back(get_type_argument());
	}
	// extra args, as specified manually in CLI or when adding the drive
	auto extra_args = get_extra_arguments();
	args.insert(args.end(), extra_args.begin(), extra_args.end());

	// config options, as specified in preferences.
	std::vector<std::string> config_options = app_get_device_options(get_device(), get_type_argument());
	args.insert(args.end(), config_options.begin(), config_options.end());

	return args;
}



hz::ExpectedVoid<StorageDeviceError> StorageDevice::execute_device_smartctl(const std::vector<std::string>& command_options,
		const std::shared_ptr<CommandExecutor>& smartctl_ex, std::string& smartctl_output, bool check_type)
{
	// don't forbid running on currently tested drive - we need to call this from the test code.

	if (is_virtual_) {
		debug_out_warn("app", DBG_FUNC_MSG << "Cannot execute smartctl on a virtual device.\n");
		return hz::Unexpected(StorageDeviceError::CannotExecuteOnVirtual, _("Cannot execute smartctl on a virtual device."));
	}

	const std::string device = get_device();

	auto smartctl_status = execute_smartctl(device, this->get_device_options(),
			command_options, smartctl_ex, smartctl_output);

	if (!smartctl_status) {
		debug_out_warn("app", DBG_FUNC_MSG << "Smartctl binary did not execute cleanly.\n");

		// Smartctl 5.39 cvs/svn version defaults to usb type on at least linux and windows.
		// This means that the old SCSI identify command isn't executed by default,
		// and there is no information about the device manufacturer/etc. in the output.
		// We detect this and set the device type to scsi to at least have _some_ info.

		// Note: This match works even with JSON (the text output is included in --json=o).
		if (check_type && this->get_detected_type() == StorageDeviceDetectedType::Unknown
				&& app_regex_partial_match("/specify device type with the -d option/mi", smartctl_output)) {
			this->set_detected_type(StorageDeviceDetectedType::NeedsExplicitType);
		}

		return hz::Unexpected(StorageDeviceError::ExecutionError, smartctl_status.error().message());
	}

	return {};
}



sigc::signal<void, StorageDevice*>& StorageDevice::signal_changed()
{
	return signal_changed_;
}



void StorageDevice::set_parse_status(ParseStatus value)
{
	parse_status_ = value;
}


void StorageDevice::set_property_repository(StoragePropertyRepository repository)
{
	property_repository_ = std::move(repository);
}







/// @}
