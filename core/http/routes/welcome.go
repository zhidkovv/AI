package routes

import (
	"github.com/go-skynet/LocalAI/core/config"
	"github.com/go-skynet/LocalAI/internal"
	"github.com/go-skynet/LocalAI/pkg/model"
	"github.com/gofiber/fiber/v2"
)

func RegisterWelcomeRoute(
	app *fiber.App,
	cl *config.BackendConfigLoader,
	ml *model.ModelLoader,
	appConfig *config.ApplicationConfig,
	auth func(*fiber.Ctx) error,
) {
	if appConfig.DisableWelcomePage {
		return
	}

	models, _ := ml.ListModels()
	backendConfigs := cl.GetAllBackendConfigs()

	app.Get("/", auth, func(c *fiber.Ctx) error {
		summary := fiber.Map{
			"Title":             "LocalAI API - " + internal.PrintableVersion(),
			"Version":           internal.PrintableVersion(),
			"Models":            models,
			"ModelsConfig":      backendConfigs,
			"ApplicationConfig": appConfig,
		}

		if string(c.Context().Request.Header.ContentType()) == "application/json" || len(c.Accepts("html")) == 0 {
			// The client expects a JSON response
			return c.Status(fiber.StatusOK).JSON(summary)
		} else {
			// Render index
			return c.Render("views/index", summary)
		}
	})

}
