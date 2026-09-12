/*
    Write points to SubRip subtitle file (for video geotagging)

    Copyright (C) 2010 Michael von Glasow, michael @t vonglasow d.t com
    Copyright (C) 2014 Gleb Smirnoff, glebius @t FreeBSD d.t org

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

 */
#include <QDate>                // for QDate
#include <QDateTime>            // for QDateTime, operator<<
#include <QDebug>               // for QDebug
#include <QString>              // for QString
#include <QTime>                // for QTime
#include <Qt>                   // for UTC

#include "defs.h"
#include "subrip.h"
#include "src/core/datetime.h"  // for DateTime
#include "src/core/logging.h"   // for Fatal

#include <format>   // for std::vformat, std::make_format_args
#include <vector>   // for std::vector
#include <regex>    // for std::regex, std:regex_replace
#include <limits>   // for std::numeric_limits::quiet_NaN
#include <variant>
#include <iostream>
#include <map>
#include <unordered_map>
#include <ranges>

/// @brief Performs placement and formatting of values into a string. 
/// These format strings are generally like those expected by std::format(), 
//// but should normally have a field name specified similar to python's 
/// f-strings. Unlike std::format or python, curly braces do not need to 
/// be "doubled" to insert a single brace in the final formatted string.
/// A format string must specify a name for each field, such as
/// `{field_name}`, or `{field_name:0.2f}`.
class FormatString
{
public:
  using BasicValue = std::variant<int64_t, double, std::string, bool>;
  using FieldValue = std::pair<const std::string, BasicValue>;

  /// @param fmt_string A string with replacement fields.
  /// @param nan_inf_replacement A string that will replace "nan" or "inf" 
  /// when a double is converted to string.
  FormatString(const std::string &fmt_string, const std::string &nan_inf_replacement = "---")
      : original_fmt_string{fmt_string},
        nan_inf_replacement{nan_inf_replacement} {}

  ~FormatString() = default;

  /// @brief Replaces named fields in format string using the mapping of field name to value.
  /// @param fields A mapping of field names to their replacement values. This
  /// can be anything that provides an iterable of `FieldValue`.
  /// @return The format string with fields replaced.
  /// @throws std::format_error An error during field replacement.
  template <typename T>
    requires std::ranges::forward_range<T> && std::same_as<std::ranges::range_value_t<T>, FieldValue>
  std::string format(const T &fields) const
  {
    std::string format_str{original_fmt_string};

    for (const auto &[field_name, field_value] : fields)
    {
      const auto field_spec{std::format("\\{{{}(?::.*?)?\\}}", field_name)};
      const std::regex field_spec_re{field_spec};
      for (std::smatch matched_spec; std::regex_search(format_str, matched_spec, field_spec_re);)
      {
        const auto replacement = format_single_field(matched_spec.str(), field_name, field_value);
        format_str = matched_spec.prefix().str() + replacement + matched_spec.suffix().str();
      }
    }

    return format_str;
  }

private:
  const std::string original_fmt_string;

  const std::string nan_inf_replacement;
  const std::regex nan_inf_re{"nan|-?inf", std::regex_constants::icase};

  /// @brief Replace a single field, possibly containing a field name, with the
  /// formatted version of its value.
  /// @param field_spec A field such as `{}`, `{:0.2f}`, `{field_name}`, or `{field_name:0.2f}`
  /// @param field_name The name of a variable that may appear in `field spec`.
  /// @param field_value A value to be formatted according to `field_spec`.
  /// @return The final formatted value as a string.
  /// @throws std::format_error An error during field replacement.
  std::string format_single_field(std::string field_spec, const std::string &field_name, const BasicValue &field_value) const
  {
    try
    {
      field_spec = std::regex_replace(field_spec, std::regex{field_name}, "");
      auto format_variant = [&](auto &v){ return std::vformat(field_spec, std::make_format_args(v)); };
      auto formatted = std::visit(format_variant, field_value);
      formatted = std::regex_replace(formatted, nan_inf_re, nan_inf_replacement);
      return formatted;
    }
    catch (const std::format_error &e)
    {
      const std::string error = std::format("format error for {}: {}", field_name, e.what());
      throw new std::format_error{error};
    }
  }
};

/* internal helper functions */

QTime
SubripFormat::video_time(const QDateTime& dt) const
{
  return QTime::fromMSecsSinceStartOfDay(video_datetime.msecsTo(dt));
}

void
SubripFormat::subrip_prevwp_pr(const Waypoint* waypointp)
{
  static long long deltaoffset;

  /* Now that we have the next waypoint, we can write out the subtitle for
   * the previous one.
   */

  /* If this condition is not true, the waypoint is before the beginning of
   * the video and will be ignored
   */
  if (prevwpp->GetCreationTime() < video_datetime) {
    return;
  }

  *fout << QString::number(stnum++) << "\n";

  /* Writes start and end time for subtitle display to file. */
  QDateTime end_datetime;
  if (!waypointp) {
    // prevwpp is the last waypoint, so we don't have a datetime for the
    // next waypoint.  Instead, estimate it from length of the previous
    // video frame.
    end_datetime = prevwpp->GetCreationTime().addMSecs(deltaoffset);
  } else {
    end_datetime = waypointp->GetCreationTime();
    deltaoffset = prevwpp->GetCreationTime().msecsTo(waypointp->GetCreationTime());
  }
  QTime starttime = video_time(prevwpp->GetCreationTime());
  QTime endtime = video_time(end_datetime);
  *fout << QStringLiteral("%1:%2:%3,%4 --> %5:%6:%7,%8\n")
            .arg(starttime.hour(), 2, 10, QChar('0'))
              .arg(starttime.minute(), 2, 10, QChar('0'))
              .arg(starttime.second(), 2, 10, QChar('0'))
              .arg(starttime.msec(), 3, 10, QChar('0'))
            .arg(endtime.hour(), 2, 10, QChar('0'))
              .arg(endtime.minute(), 2, 10, QChar('0'))
              .arg(endtime.second(), 2, 10, QChar('0'))
              .arg(endtime.msec(), 3, 10, QChar('0'));

  *fout << subrip_format() << "\n\n";
}

QString
SubripFormat::subrip_format()
{
  // prepare fields and values
  const auto nan_inf_replacement = "---";
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const QTime t = prevwpp->GetCreationTime().toUTC().time();
  const double speed_factor = opt_speedfactor.has_value() ? opt_speedfactor.get_result() : 1.0;
  const double altitude_factor = opt_altitudefactor.has_value() ? opt_altitudefactor.get_result() : 1.0;

  const std::unordered_map<std::string, FormatString::BasicValue> fields = {
    {"hour", t.hour()},
    {"minute", t.minute()},
    {"second", t.second()},
    {"longitude", prevwpp->longitude},
    {"latitude", prevwpp->latitude},
    {"altitude", prevwpp->altitude != unknown_alt ? prevwpp->altitude * altitude_factor : nan},
    {"speed", prevwpp->speed_has_value() ? speed_factor * prevwpp->speed_value() : nan},
    {"course", prevwpp->course_has_value() ? prevwpp->course_value() : nan},
    {"vspeed", vspeed.has_value() ? *vspeed * altitude_factor : nan},
    {"gradient", gradient.has_value() ? *gradient : nan},
    {"cadence", prevwpp->cadence != 0 ? prevwpp->cadence : nan},
    {"heartrate", prevwpp->heartrate != 0 ? prevwpp->heartrate : nan},
    {"turd", "666"},
    {"butt", true},
  };

  try
  {
    return QString::fromStdString( FormatString(opt_format.get().toStdString()).format(fields) );
  }
  catch (const std::format_error &e)
  {
    gbFatal(e.what());
  }

  // std::vector<BasicValue> values = {
  //   t.hour(),
  //   t.minute(),
  //   t.second(),
  //   prevwpp->longitude,
  //   prevwpp->latitude,
  //   prevwpp->altitude != unknown_alt ? prevwpp->altitude * altitude_factor : nan,
  //   prevwpp->speed_has_value() ? speed_factor * prevwpp->speed_value() : nan,
  //   prevwpp->course_has_value() ? prevwpp->course_value() : nan,
  //   vspeed.has_value() ? *vspeed * altitude_factor: nan,
  //   gradient.has_value() ? *gradient : nan,
  //   prevwpp->cadence != 0 ? prevwpp->cadence : nan,
  //   prevwpp->heartrate != 0 ? prevwpp->heartrate : nan,
  //   "666",
  // };

  // for each kv,
  // create fmt regex with var name
  // do regex search loop
  //  when find fmt exp, 
  //    replace var name with {}
  //    run std::format
  //    re
  //   

  // replace field names with indexes
  /* the good stuff
  std::string user_fmt{opt_format.get().toStdString()};
  const std::regex nan_inf_re{"nan|-?inf", std::regex_constants::icase};

  for (const auto& [field_name, field_value]: fields)
  {
    const auto f{std::format("\\{{{}(?::.*?)?\\}}", field_name)};
    const std::regex find_re{f};
    for (std::smatch found; std::regex_search(user_fmt, found, find_re);)
    {
      auto found_fmt = found.str();
      found_fmt = std::regex_replace(found_fmt, std::regex{field_name}, "");
      try
      {
        auto stupid_shit = [&](auto &v) { return std::vformat(found_fmt, std::make_format_args(v)); };
        auto formatted = std::visit(stupid_shit, field_value);
        formatted = std::regex_replace(formatted, nan_inf_re, nan_inf_replacement);
        user_fmt = found.prefix().str() + formatted + found.suffix().str();
      }
      catch (const std::format_error &e)
      {
        const std::string error = std::format("format error for {}: {}", field_name, e.what());
        gbFatal(error.c_str());
      }
    }
  }*/

  // std::cout << "b4 braces " << user_fmt << "\n";
  // dedouble braces like normal format
  // const std::regex open_re{"(\\{\\{)+"};
  // const std::regex close_re{"(\\}\\})+"};
  // user_fmt = std::regex_replace(user_fmt, open_re, "{");
  // user_fmt = std::regex_replace(user_fmt, close_re, "}");
  // std::cout << "final " << user_fmt << "\n";

  // find and error on any unknown leftover format expressions
  // because these will crash the actual replacement
  // const std::regex unknown_re{"\\{+(\\D*?)(:.*?)?\\}+"};
  // std::string unknown_fmt;
  // std::string rest = user_fmt;
  // for (std::smatch bad; std::regex_search(rest, bad, unknown_re);)
  // {
  //   unknown_fmt += " ";
  //   unknown_fmt += bad[0];
  //   rest = bad.suffix();
  // }
  // if (unknown_fmt != "")
  // {
  //   const std::string error = "ERROR: unknown formats:" + unknown_fmt;
  //   gbFatal(error.c_str());
  // }

  // return QString::fromStdString(user_fmt);

  // // stupidly fill the args
  // const auto args = std::make_format_args(
  //   values[0], values[1], values[2], values[3], values[4], values[5],
  //   values[6], values[7], values[8], values[9], values[10], values[11]);
  // // do the actual thing
  // try
  // {
  //   const auto final_fmt = std::vformat(user_fmt, args);
  //   return QString::fromStdString(final_fmt);
  // }
  // catch(const std::format_error& e)
  // {
  //   const std::string error = std::format("format error: {}", e.what());
  //   gbFatal(error.c_str()); 
  // }
}

/* callback functions */

void
SubripFormat::subrip_trkpt_pr(const Waypoint* waypointp)
{
  /*
   * To determine the duration of the subtitle, we need the timestamp of the
   * associated waypoint plus that of the following one.
   * Since we get waypoints one at a time, the only way is to store one and
   * defer processing until we get the next one.
   *
   * To determine vertical speed we need to have not only previous waypoint,
   * but also pre-previous, so we calculate vspeed right before forgetting
   * the previous.
   */
  if (!video_datetime.isValid()) {
    if (!gps_datetime.isValid()) {
      // If gps_date and gps_time options weren't used, then we use the
      // datetime of the first waypoint to sync to the video.
      gps_datetime = waypointp->GetCreationTime().toUTC();
    }
    video_datetime = gps_datetime.addMSecs(-video_offset_ms).toUTC();
    if (global_opts.debug_level >= 2) {
      qDebug().noquote() << "GPS track start is           "
                         << waypointp->GetCreationTime().toUTC().toString(Qt::ISODateWithMs);
      qDebug().noquote() << "Synchronizing"
                         << video_time(gps_datetime).toString(u"HH:mm:ss,zzz")
                         << "to" << gps_datetime.toString(Qt::ISODateWithMs);
      qDebug().noquote() << "Video start   00:00:00,000 is"
                         << video_datetime.toString(Qt::ISODateWithMs);
    }
  }

  if (prevwpp) {
    subrip_prevwp_pr(waypointp);
    vspeed = waypt_vertical_speed(waypointp, prevwpp);
    gradient = waypt_gradient(waypointp, prevwpp);
  }
  prevwpp = waypointp;
}

/* global callback (exported) functions */

void
SubripFormat::wr_init(const QString& fname)
{
  stnum = 1;
  prevwpp = nullptr;
  vspeed.reset();
  gradient.reset();

  if (opt_gpstime != opt_gpsdate) {
    gbFatal(FatalMsg() << "Either both or neither of the gps_date and gps_time options must be supplied!");
  }
  gps_datetime = QDateTime();
  if (opt_gpstime && opt_gpsdate) {
    QDate gps_date = QDate::fromString(opt_gpsdate, u"yyyyMMdd");
    if (!gps_date.isValid()) {
      gbFatal(FatalMsg().nospace() << "option gps_date value (" << opt_gpsdate.get() << ") is invalid.  Expected yyyymmdd.");
    }
    QTime gps_time = QTime::fromString(opt_gpstime, u"HHmmss");
    if (!gps_time.isValid()) {
      gps_time = QTime::fromString(opt_gpstime, u"HHmmss.z");
      if (!gps_time.isValid()) {
        gbFatal(FatalMsg().nospace() << "option gps_time value (" << opt_gpstime.get() << ") is invalid.  Expected hhmmss[.sss]");
      }
    }
    gps_datetime = QDateTime(gps_date, gps_time, QtUTC);
  }

  video_offset_ms = 0;
  if (opt_videotime) {
    QTime video_time = QTime::fromString(opt_videotime, u"HHmmss");
    if (!video_time.isValid()) {
      video_time = QTime::fromString(opt_videotime, u"HHmmss.z");
      if (!video_time.isValid()) {
        gbFatal(FatalMsg().nospace() << "option video_time value (" << opt_videotime.get() << ") is invalid.  Expected hhmmss[.sss].");
      }
    }
    video_offset_ms = video_time.msecsSinceStartOfDay();
  }

  video_datetime = QDateTime();

  fout = new gpsbabel::TextStream;
  fout->open(fname, QIODevice::WriteOnly);
}

void
SubripFormat::wr_deinit()
{
  fout->close();
  delete fout;
  fout = nullptr;
}

void
SubripFormat::write()
{
  auto subrip_trkpt_pr_lambda = [this](const Waypoint* waypointp)->void {
    subrip_trkpt_pr(waypointp);
  };
  track_disp_all(nullptr, nullptr, subrip_trkpt_pr_lambda);

  /*
   * Due to the necessary hack, one waypoint is still in memory (unless we
   * did not get any waypoints). Check if there is one and, if so, write it.
   */
  if (prevwpp) {
    subrip_prevwp_pr(nullptr);
  }
}
