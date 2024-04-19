package routes

import (
	"github.com/go-skynet/LocalAI/core/config"
	"github.com/go-skynet/LocalAI/internal"
	"github.com/go-skynet/LocalAI/pkg/model"
	"github.com/gofiber/fiber/v2"
)

func RegisterChatRoute(
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

	app.Get("/chat", auth, func(c *fiber.Ctx) error {
		summary := fiber.Map{
			"Title":             "LocalAI API - " + internal.PrintableVersion(),
			"Version":           internal.PrintableVersion(),
			"Models":            models,
			"ModelsConfig":      backendConfigs,
			"ApplicationConfig": appConfig,
		}

		// Render index
		return c.Render("views/chat", summary)

	})

}
