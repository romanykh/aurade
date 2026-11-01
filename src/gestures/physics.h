#ifndef AURADE_PHYSICS_H
#define AURADE_PHYSICS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Spring configuration parameters matching macOS CoreAnimation physics.
 */
typedef struct {
    double mass;                 // Default: 1.0
    double stiffness;            // Default: 280.0
    double damping;              // Damping ratio zeta, Default: 0.92
    double velocity_threshold;   // Minimum velocity to be considered settled
    double position_threshold;   // Minimum distance to be considered settled
} spring_config_t;

/**
 * Active spring simulation state.
 */
typedef struct {
    spring_config_t config;
    double start_pos;
    double target_pos;
    double initial_velocity;
    double current_pos;
    double current_vel;
    double elapsed_time;         // Time in seconds since release
    bool settled;
} spring_simulation_t;

/**
 * Initialize default physics configuration.
 */
spring_config_t spring_config_default(void);

/**
 * Initialize a new spring simulation.
 * @param sim Simulation instance to initialize
 * @param config Physics parameters
 * @param start_pos Initial position at finger release
 * @param target_pos Destination position to snap towards
 * @param initial_velocity Velocity at finger release (units/second)
 */
void spring_simulation_start(spring_simulation_t *sim, const spring_config_t *config,
                             double start_pos, double target_pos, double initial_velocity);

/**
 * Step the spring simulation forward by dt seconds.
 * Evaluates the exact analytical second-order ODE solution.
 * @return True if still animating, false if settled at target.
 */
bool spring_simulation_step(spring_simulation_t *sim, double dt);

/**
 * Compute macOS rubber-band elastic displacement when dragging past limits.
 * @param displacement Raw finger displacement past the boundary (can be positive or negative)
 * @param limit_constant Elastic resistance coefficient K (e.g. 200.0)
 * @return Non-linear resisted displacement
 */
double rubber_band_calculate(double displacement, double limit_constant);

#endif // AURADE_PHYSICS_H
