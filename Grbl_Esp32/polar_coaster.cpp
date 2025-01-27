/*
  kinematics_polar_coaster.cpp - Implements simple inverse kinematics for Grbl_ESP32
  Part of Grbl_ESP32

  Copyright (c) 2019 Barton Dring @buildlog


  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

	Inverse kinematics determine the joint parameters required to get to a position
	in 3D space. Grbl will still work as 3 axes of steps, but these steps could
	represent angles, etc instead of linear units.

	Unless forward kinematics are applied to the reporting, Grbl will report raw joint
	values instead of the normal Cartesian positions

	How it works...

	If you tell it to go to X10 Y10 Z10 in Cartesian space, for example, the equations
  will convert those values to the required joint values. In the case of a polar machine, X represents the radius,
	Y represents the polar degrees and Z would be unchanged.

	In most cases, a straight line in Cartesian space could cause a curve in the new system.
	To fix this, the line is broken into very small segments and each segment is converted
	to the new space. While each segment is also distorted, the amount is so small it cannot be seen.

	This segmentation is how normal Grbl draws arcs.

	Feed Rate

	Feed rate is given in steps/time. Due to the new coordinate units and non linearity issues, the
	feed rate may need to be adjusted. The ratio of the step distances in the original coordinate system
	determined and applied to the feed rate.

	TODO:
		Add y offset, for completeness
		Add ZERO_NON_HOMED_AXES option


*/
#include "grbl.h"
#ifdef CPU_MAP_POLAR_COASTER
#ifdef USE_KINEMATICS

// this get called before homing 
// return true to complete normal home
// return false to exit normal homing
bool kinematics_homing(uint8_t cycle_mask) 
{
	// cycle mask not used for polar coaster	
	
	// zero the axes that are not homed
	sys_position[Y_AXIS] = 0.0f; 
	sys_position[Z_AXIS] = 0.0f; 

	return true; // finish normal homing cycle
}

/*
 Apply inverse kinematics for a polar system

 float target: 					The desired target location in machine space
 plan_line_data_t *pl_data:		Plan information like feed rate, etc
 float *position:				The previous "from" location of the move

 Note: It is assumed only the radius axis (X) is homed and only X and Z have offsets


*/
void inverse_kinematics(float *target, plan_line_data_t *pl_data, float *position)
{
	static float last_angle = 0;
	static float last_radius = 0;

	float dx, dy, dz; // distances in each cartesian axis
	float p_dx, p_dy, p_dz; // distances in each polar axis

	float dist, polar_dist; // the distances in both systems...used to determine feed rate
	float new_feedrate;  //

	uint32_t segment_count;  // number of segments the move will be broken in to.
	float seg_target[N_AXIS]; // The target of the current segment

	float polar[N_AXIS]; // target location in polar coordinates

	float x_offset = gc_state.coord_system[X_AXIS]+gc_state.coord_offset[X_AXIS]; // offset from machine coordinate system
	float z_offset = gc_state.coord_system[Z_AXIS]+gc_state.coord_offset[Z_AXIS]; // offset from machine coordinate system

	//grbl_sendf(CLIENT_SERIAL, "Position: %4.2f %4.2f %4.2f \r\n", position[X_AXIS] - x_offset, position[Y_AXIS], position[Z_AXIS]);
	//grbl_sendf(CLIENT_SERIAL, "Target: %4.2f %4.2f %4.2f \r\n", target[X_AXIS] - x_offset, target[Y_AXIS], target[Z_AXIS]);

	// calculate cartesian move distance for each axis
	dx = target[X_AXIS] - position[X_AXIS];
	dy = target[Y_AXIS] - position[Y_AXIS];
	dz = target[Z_AXIS] - position[Z_AXIS];

	// calculate the total X,Y axis move distance
	// Z axis is the same in both coord systems, so it is ignored
	dist = sqrt( (dx * dx) + (dy * dy) + (dz * dz));

	if (pl_data->condition & PL_COND_FLAG_RAPID_MOTION) {
		segment_count = 1;    // rapid G0 motion is not used to draw, so skip the segmentation
	} else {
		segment_count = ceil(dist / SEGMENT_LENGTH);    // determine the number of segments we need	... round up so there is at least 1
	}

	dist /= segment_count;  // segment distance


	for(uint32_t segment = 1; segment <= segment_count; segment++) {
		// determine this segment's target
		seg_target[X_AXIS] = position[X_AXIS] + (dx / float(segment_count) * segment) - x_offset;
		seg_target[Y_AXIS] = position[Y_AXIS] + (dy / float(segment_count) * segment);
		seg_target[Z_AXIS] = position[Z_AXIS] + (dz / float(segment_count) * segment) - z_offset;

		calc_polar(seg_target, polar, last_angle);
		
		// begin determining new feed rate
		
		// calculate move distance for each axis
		p_dx = polar[RADIUS_AXIS] - last_radius;
		p_dy = polar[POLAR_AXIS] - last_angle;
		p_dz = dz;

		polar_dist = sqrt( (p_dx * p_dx) + (p_dy * p_dy) +(p_dz * p_dz)); // calculate the total move distance

		if (polar_dist == 0 || dist == 0) { // prevent 0 feed rate
			polar_dist = dist;    // default to same feed rate
		}

		pl_data->feed_rate *= polar_dist / dist;  // apply the distance ratio between coord systems
		
		// end determining new feed rate 

		polar[RADIUS_AXIS] += x_offset;
		polar[Z_AXIS] += z_offset;

		last_radius = polar[RADIUS_AXIS];
		last_angle = polar[POLAR_AXIS];

		//grbl_sendf(CLIENT_SERIAL, "Polar: %4.2f \r\n", polar[POLAR_AXIS]);
		//grbl_sendf(CLIENT_SERIAL, "Radius: %4.2f \r\n", polar[RADIUS_AXIS]);

		mc_line(polar, pl_data);
	}
	
	// TO DO don't need a feedrate for rapids
}


/*
Forward kinematics converts position back to the original cartesian system. It is
typically used for reporting

For example, on a polar machine, you tell it to go to a place like X10Y10. It 
converts to a radius and angle using inverse kinematics. The machine posiiton is now 
in those units X14.14 (radius) and Y45 (degrees). If you want to report those units as 
X10,Y10, you would use forward kinematics

position = the current machine position
converted = position with forward kinematics applied.

*/
void forward_kinematics(float *position)
{
	float original_position[N_AXIS]; // temporary storage of original
	float print_position[N_AXIS];
	int32_t current_position[N_AXIS]; // Copy current state of the system position variable
	
	memcpy(current_position,sys_position,sizeof(sys_position));
	system_convert_array_steps_to_mpos(print_position,current_position);
	
	original_position[X_AXIS] = print_position[X_AXIS] - gc_state.coord_system[X_AXIS]+gc_state.coord_offset[X_AXIS];
	original_position[Y_AXIS] = print_position[Y_AXIS] - gc_state.coord_system[Y_AXIS]+gc_state.coord_offset[Y_AXIS];
	original_position[Z_AXIS] = print_position[Z_AXIS] - gc_state.coord_system[Z_AXIS]+gc_state.coord_offset[Z_AXIS];
	
	position[X_AXIS] = cos(radians(original_position[Y_AXIS])) * original_position[X_AXIS] * -1;
	position[Y_AXIS] = sin(radians(original_position[Y_AXIS])) * original_position[X_AXIS];
	position[Z_AXIS] = original_position[Z_AXIS];  // unchanged
}

// helper functions

/*******************************************
* Calculate polar values from Cartesian values
* 	float target_xyz:	An array of target axis positions in Cartesian (xyz) space
*		float polar: 			An array to return the polar values
*		float last_angle:	The polar angle of the "from" point.
*
*   Angle calculated is 0 to 360, but you don't want a line to go from 350 to 10. This would
*   be a long line backwards. You want it to go from 350 to 370. The same is true going the other way.
*
*   This means the angle could accumulate to very high positive or negative values over the coarse of
*   a long job.
*
*/
void calc_polar(float *target_xyz, float *polar, float last_angle)
{
	float delta_ang; // the difference from the last and next angle
	
	//grbl_sendf(CLIENT_SERIAL, "calc polar: x...%4.2f y...%4.2f\r\n", target_xyz[X_AXIS], target_xyz[Y_AXIS]);
	
	//target_xyz[X_AXIS] *= -1.0;   // compensate for Polar Coaster's radial axis being mirrored (right side) from normal 0deg
	
	polar[RADIUS_AXIS] = hypot_f(target_xyz[X_AXIS], target_xyz[Y_AXIS]);
	
	if (polar[RADIUS_AXIS] == 0) {
		polar[POLAR_AXIS] = last_angle; // don't care about angle at center
	}
	else {
		polar[POLAR_AXIS] = atan2(target_xyz[Y_AXIS], target_xyz[X_AXIS]) * 180.0 / M_PI;
		// no negative angles...we want the absolute angle not -90, use 270
		if (polar[POLAR_AXIS] < 0.0) {
			polar[POLAR_AXIS] = 360.0 + polar[POLAR_AXIS];
		}
	}
	
	

	polar[Z_AXIS] = target_xyz[Z_AXIS]; // Z is unchanged

	delta_ang = polar[POLAR_AXIS] - fmod(last_angle, 360.0); // what is the difference from the last angle

	// if the delta is above 180 degrees it means we are crossing the 0 degree line
	if ( fabs(delta_ang) <= 180.0) {
		polar[POLAR_AXIS] = last_angle + delta_ang;
	} else {
		if (fmod(last_angle, 360.0) > 180.0) {  // crossing zer clockwise
			polar[POLAR_AXIS]  = last_angle + delta_ang + 360.0;
		} else { // rossing zero counter clockwise
			polar[POLAR_AXIS] = last_angle - (360.0 - delta_ang);
		}
	}
}
#endif

// Polar coaster has macro buttons, this handles those button pushes.
void user_defined_macro(uint8_t index)
{
	//grbl_sendf(CLIENT_SERIAL, "[MSG: Macro #%d]\r\n", index);
	switch (index) {
		#ifdef MACRO_BUTTON_0_PIN
		case CONTROL_PIN_INDEX_MACRO_0:
			inputBuffer.push("$H\r"); // home machine
		break;
		#endif
		#ifdef MACRO_BUTTON_1_PIN
		case CONTROL_PIN_INDEX_MACRO_1:
			inputBuffer.push("[ESP220]/1.nc\r"); // run SD card file 1.nc
		break;
		#endif
		#ifdef MACRO_BUTTON_2_PIN
		case CONTROL_PIN_INDEX_MACRO_2:
			inputBuffer.push("[ESP220]/2.nc\r"); // run SD card file 2.nc
		break;
		#endif
		#ifdef MACRO_BUTTON_3_PIN
		case CONTROL_PIN_INDEX_MACRO_3:
		break;
		#endif
		default:
		break;
	}
}
#endif

